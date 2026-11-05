import java.time.Duration

plugins {
	`java-library`
	`maven-publish`
	signing
	alias(libs.plugins.nmcp)
}

evaluationDependsOn(":webrtc")

val platformClassifier = rootProject.extra["platformClassifier"] as String
val buildDate = rootProject.extra["buildDate"] as String
val nativesDir = rootProject.extra["nativesDir"] as File?
val dataChannelsOnly = rootProject.extra["dataChannelsOnly"] as Boolean

description = "Swing extension for webrtc-java. Shows the video of the client API in Swing windows."

val moduleName = "io.github.sendablemetatype.webrtc.swing"
val coreModuleName = "io.github.sendablemetatype.webrtc"

base {
	archivesName = "webrtc-java-swing"
}

java {
	withSourcesJar()
	withJavadocJar()
}

dependencies {
	api(project(":webrtc"))

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
	options.release = 25
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

tasks.javadoc {
	title = "webrtc-java-swing ${project.version} API"
	(options as StandardJavadocDocletOptions).apply {
		encoding = "UTF-8"
		author(true)
		version(true)
		use(true)
		addStringOption("Xdoclint:none", "-quiet")
	}
	isFailOnError = false
}

tasks.test {
	useJUnitPlatform()

	// The view shows the video of the full variant.
	onlyIf("the full variant is built") { !dataChannelsOnly }

	// Opt-in check of every JNI call's references, see docs/guide/build.md.
	if (providers.gradleProperty("jni-check").isPresent) {
		jvmArgs("-Xcheck:jni")
	}
	dependsOn(tasks.jar, project(":webrtc").tasks.named("jar"))

	// A hung test JVM fails the build instead of blocking a CI runner.
	timeout = Duration.ofMinutes(30)

	testLogging {
		events("passed", "skipped", "failed")
	}

	// The tests run inside the module, as in the webrtc project: both module
	// jars on the module path, the test classes patched into this module, the
	// native library and JUnit on the class path.
	val coreBuildDir = project(":webrtc").layout.buildDirectory.get().asFile
	classpath = configurations.testRuntimeClasspath.get().filter { !it.startsWith(coreBuildDir) }

	val moduleJar = tasks.jar.flatMap { it.archiveFile }
	val testOutput = sourceSets.test.map { it.output }
	inputs.files(testOutput).withPropertyName("testOutput")
	val coreModuleJar = project(":webrtc").tasks.named<Jar>("jar").flatMap { it.archiveFile }

	jvmArgumentProviders.add(CommandLineArgumentProvider {
		val patch = testOutput.get().filter { it.exists() }.asPath

		listOf(
			"--module-path", listOf(moduleJar.get().asFile, coreModuleJar.get().asFile).joinToString(File.pathSeparator),
			"--add-modules", moduleName,
			"--patch-module", "$moduleName=$patch",
			"--add-reads", "$moduleName=ALL-UNNAMED",
			"--add-opens", "$moduleName/io.github.sendablemetatype.webrtc.client.swing=ALL-UNNAMED",
			"--enable-native-access=$coreModuleName",
		)
	})
}

// Gradle module metadata would bypass the parent pom, as in the webrtc project.
tasks.withType<GenerateModuleMetadata>().configureEach {
	enabled = false
}

// Published with the full variant only, whose video it shows.
if (!dataChannelsOnly) {
	publishing {
		publications {
			create<MavenPublication>("maven") {
				artifactId = "webrtc-java-swing"

				from(components["java"])

				pom {
					name = "webrtc-java-swing"
					description = project.description

					withXml {
						val root = asNode()

						val parent = groovy.util.Node(null, "parent")
						parent.appendNode("groupId", project.group)
						parent.appendNode("artifactId", "webrtc-java-parent")
						parent.appendNode("version", project.version)
						@Suppress("UNCHECKED_CAST")
						(root.children() as MutableList<Any>).add(1, parent)
					}
				}
			}
		}
	}
}
