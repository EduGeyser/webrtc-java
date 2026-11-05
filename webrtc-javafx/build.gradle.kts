plugins {
	`java-library`
	`maven-publish`
	signing
	alias(libs.plugins.nmcp)
}

val buildDate = rootProject.extra["buildDate"] as String
val dataChannelsOnly = rootProject.extra["dataChannelsOnly"] as Boolean
val javafxClassifier = rootProject.extra["javafxClassifier"] as String

description = "JavaFX extension for webrtc-java. Shows the video of the client API in JavaFX scenes."

base {
	archivesName = "webrtc-java-javafx"
}

java {
	withSourcesJar()
	withJavadocJar()
}

dependencies {
	api(project(":webrtc"))

	// Not a dependency of the published module: an application that uses it
	// has JavaFX already, in the version and for the platforms it chose.
	compileOnly(variantOf(libs.javafx.base) { classifier(javafxClassifier) })
	compileOnly(variantOf(libs.javafx.graphics) { classifier(javafxClassifier) })

	testImplementation(variantOf(libs.javafx.base) { classifier(javafxClassifier) })
	testImplementation(variantOf(libs.javafx.graphics) { classifier(javafxClassifier) })
	testImplementation(platform(libs.junit.bom))
	testImplementation(libs.junit.jupiter.api)
	testRuntimeOnly(libs.junit.jupiter.engine)
	testRuntimeOnly(libs.junit.platform.launcher)
	testRuntimeOnly(project(path = ":webrtc-jni", configuration = "natives"))
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
	title = "webrtc-java-javafx ${project.version} API"
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

	// The view shows the video of the full variant. The tests need a display
	// for the JavaFX toolkit and skip where there is none, as on CI runners.
	onlyIf("the full variant is built") { !dataChannelsOnly }

	testLogging {
		events("passed", "skipped", "failed")
	}
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
				artifactId = "webrtc-java-javafx"

				from(components["java"])

				pom {
					name = "webrtc-java-javafx"
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
