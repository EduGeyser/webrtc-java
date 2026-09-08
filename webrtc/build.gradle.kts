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

val moduleName = "webrtc.java"
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
}

tasks.compileJava {
	// Java 8 class files. module-info.java needs Java 9 and is compiled separately.
	options.release = 8
	exclude("module-info.java")
}

val compileModuleInfo = tasks.register<JavaCompile>("compileModuleInfo") {
	description = "Compiles module-info.java for Java 9. Only module-info.class of this compilation is used."
	source(sourceSets.main.map { it.allJava })
	classpath = files()
	options.release = 9
	options.encoding = "UTF-8"
	destinationDirectory = layout.buildDirectory.dir("classes/java/module-info")
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
	from(compileModuleInfo) {
		include("module-info.class")
	}

	manifest {
		attributes(
			"Version" to project.version,
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
	exclude("dev/onvoid/webrtc/internal/**")
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
	useJUnitPlatform()
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

	jvmArgumentProviders.add(CommandLineArgumentProvider {
		val patch = testOutput.get().filter { it.exists() }.asPath
		val opens = listOf(
			"dev.onvoid.webrtc",
			"dev.onvoid.webrtc.logging",
			"dev.onvoid.webrtc.media",
			"dev.onvoid.webrtc.media.audio",
			"dev.onvoid.webrtc.media.video",
		)

		listOf(
			"--module-path", moduleJar.get().asFile.absolutePath,
			"--add-modules", moduleName,
			"--patch-module", "$moduleName=$patch",
			"--add-reads", "$moduleName=ALL-UNNAMED",
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
