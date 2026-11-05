import java.time.Duration

plugins {
	`java-library`
	`maven-publish`
	signing
	alias(libs.plugins.nmcp)
}

evaluationDependsOn(":webrtc-jni")

val platformClassifier = rootProject.extra["platformClassifier"] as String
val buildDate = rootProject.extra["buildDate"] as String
val nativesDir = rootProject.extra["nativesDir"] as File?
val dataChannelsOnly = rootProject.extra["dataChannelsOnly"] as Boolean

val moduleName = "io.github.sendablemetatype.webrtc"
val nativeClassifiers = listOf(
	"windows-x86_64", "windows-aarch64",
	"linux-x86_64", "linux-aarch64", "linux-aarch32",
	"macos-x86_64", "macos-aarch64",
)

base {
	archivesName = "webrtc-java"
}

java {
	withSourcesJar()
	withJavadocJar()
}

dependencies {
	testImplementation(platform(libs.junit.bom))
	testImplementation(libs.junit.jupiter.api)
	testRuntimeOnly(libs.junit.jupiter.engine)
	testRuntimeOnly(libs.junit.platform.launcher)

	if (nativesDir != null) {
		testRuntimeOnly(files(nativesDir.resolve("webrtc-java-${project.version}-$platformClassifier.jar")))
	}
	else {
		testRuntimeOnly(project(path = ":webrtc-jni", configuration = "natives"))
	}
}

tasks.withType<JavaCompile>().configureEach {
	options.encoding = "UTF-8"
	// Java 25 is the first LTS release with the final Foreign Function and
	// Memory API.
	options.release = 25
}

val pomProperties = tasks.register("pomProperties") {
	description = "Writes the pom.properties packaged into the jar."
	val file = layout.buildDirectory.file("maven/pom.properties")
	val content = "groupId=${project.group}\nartifactId=webrtc-java\nversion=${project.version}\n"
	inputs.property("content", content)
	outputs.file(file)
	doLast {
		file.get().asFile.writeText(content)
	}
}

tasks.jar {
	manifest {
		attributes(
			"Version" to project.version,
			"Implementation-Version" to project.version,
			"Build-Date" to buildDate,
		)
	}

}

// The Maven descriptor, as tools reading it from jars expect.
listOf(tasks.jar, tasks.named<Jar>("sourcesJar")).forEach { jar ->
	jar.configure {
		into("META-INF/maven/${project.group}/webrtc-java") {
			from(tasks.named("generatePomFileForMavenPublication")) {
				rename { "pom.xml" }
			}
			from(pomProperties)
		}
	}
}

tasks.javadoc {
	title = "webrtc-java ${project.version} API"
	// Exported packages only, as javadoc documents a module by default. The
	// internal package is patched into the module from its class files, so the
	// documented classes can still refer to it.
	exclude("io/github/sendablemetatype/webrtc/internal/**")
	(options as StandardJavadocDocletOptions).apply {
		encoding = "UTF-8"
		addStringOption("-patch-module", "$moduleName=${sourceSets.main.get().output.classesDirs.asPath}")
		author(true)
		version(true)
		use(true)
		addStringOption("Xdoclint:none", "-quiet")
	}
	isFailOnError = false
}

tasks.test {
	useJUnitPlatform {
		if (dataChannelsOnly) {
			// The native library has no audio and video.
			excludeTags("media")
		}
	}
	systemProperty("webrtc.variant", if (dataChannelsOnly) "data-channels" else "full")
	// The logging tests need messages for their sinks, which WebRTC hands on
	// only at or above the severity set when the library is loaded.
	systemProperty("io.github.sendablemetatype.webrtc.logging.severity", "INFO")

	// Opt-in check of every JNI call's references, see docs/guide/build.md.
	if (providers.gradleProperty("jni-check").isPresent) {
		jvmArgs("-Xcheck:jni")
	}
	dependsOn(tasks.jar)

	// A hung test JVM fails the build instead of blocking a CI runner.
	timeout = Duration.ofMinutes(30)

	testLogging {
		events("passed", "skipped", "failed")
	}

	// The tests run inside the module, as Surefire ran them: the jar on the
	// module path, the test classes patched into the module, JUnit on the
	// class path. The class path therefore holds the dependencies only.
	classpath = configurations.testRuntimeClasspath.get()

	val moduleJar = tasks.jar.flatMap { it.archiveFile }
	val testOutput = sourceSets.test.map { it.output }
	// The patched module includes the test resources, which the replaced class
	// path no longer brings along.
	inputs.files(testOutput).withPropertyName("testOutput")

	jvmArgumentProviders.add(CommandLineArgumentProvider {
		val patch = testOutput.get().filter { it.exists() }.asPath
		// JUnit creates the test classes reflectively, so every package that
		// holds one is opened, internal ones included. Taken from the compiled
		// tests, so that a new test package needs no entry here.
		val opens = testOutput.get().classesDirs.filter { it.isDirectory }.flatMap { dir ->
			dir.walk().filter { it.extension == "class" }
				.map { it.parentFile.relativeTo(dir).invariantSeparatorsPath.replace('/', '.') }.toList()
		}.distinct()

		// The module does not read java.desktop, but a desktop capture test
		// opens an AWT window of its own to capture.
		listOf(
			"--module-path", moduleJar.get().asFile.absolutePath,
			"--add-modules", "$moduleName,java.desktop",
			"--patch-module", "$moduleName=$patch",
			"--add-reads", "$moduleName=ALL-UNNAMED",
			"--add-reads", "$moduleName=java.desktop",
			"--enable-native-access=$moduleName",
		) + opens.flatMap { listOf("--add-opens", "$moduleName/$it=ALL-UNNAMED") }
	})
}

// Maven consumers get the native library of their platform through the parent
// pom, see the root build script. Gradle module metadata would bypass the pom.
tasks.withType<GenerateModuleMetadata>().configureEach {
	enabled = false
}

val checkNatives = tasks.register("checkNatives") {
	description = "Checks that natives.dir holds the native library jar of every platform."
	val dir = nativesDir
	val version = project.version.toString()

	onlyIf("natives.dir is given") { dir != null }
	doLast {
		val missing = nativeClassifiers.filter { !dir!!.resolve("webrtc-java-$version-$it.jar").isFile }

		if (missing.isNotEmpty()) {
			throw GradleException("Missing native library jars in $dir: ${missing.joinToString()}")
		}
	}
}

publishing {
	publications {
		create<MavenPublication>("maven") {
			artifactId = "webrtc-java"

			from(components["java"])

			if (nativesDir != null) {
				// A release: the native library jars of every platform, collected
				// from the per-platform builds. Maven Central publishes a version as
				// one atomic bundle, hence all classifiers go into one deployment.
				nativeClassifiers.forEach { nativeClassifier ->
					artifact(nativesDir.resolve("webrtc-java-${project.version}-$nativeClassifier.jar")) {
						classifier = nativeClassifier
						builtBy(checkNatives)
					}
				}
			}
			else {
				// The native library jar webrtc-jni has just built for this platform.
				artifact(project(":webrtc-jni").tasks.named<Jar>("nativeJar")) {
					classifier = platformClassifier
				}
			}

			pom {
				name = "webrtc-java"

				withXml {
					val root = asNode()

					val parent = groovy.util.Node(null, "parent")
					parent.appendNode("groupId", project.group)
					parent.appendNode("artifactId", "webrtc-java-parent")
					parent.appendNode("version", project.version)
					@Suppress("UNCHECKED_CAST")
					(root.children() as MutableList<Any>).add(1, parent)

					// Resolved by the consumer's Maven through the parent's profiles.
					val dependency = root.appendNode("dependencies").appendNode("dependency")
					dependency.appendNode("groupId", project.group)
					dependency.appendNode("artifactId", "webrtc-java")
					dependency.appendNode("version", project.version)
					dependency.appendNode("classifier", "\${platform.classifier}")
				}
			}
		}
	}
}
