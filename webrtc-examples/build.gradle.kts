plugins {
	java
}

val platformClassifier = rootProject.extra["platformClassifier"] as String
val nativesDir = rootProject.extra["nativesDir"] as File?

base {
	archivesName = "webrtc-java-examples"
}

dependencies {
	implementation(project(":webrtc"))
	implementation(libs.jetty.server)
	implementation(libs.jetty.websocket.server)
	implementation(libs.jetty.util)
	implementation(libs.jackson.databind)
	implementation(libs.slf4j.jdk14)

	if (nativesDir != null) {
		runtimeOnly(files(nativesDir.resolve("webrtc-java-${project.version}-$platformClassifier.jar")))
	}
	else {
		runtimeOnly(project(path = ":webrtc-jni", configuration = "natives"))
	}
}

tasks.withType<JavaCompile>().configureEach {
	options.encoding = "UTF-8"
	options.release = 25
}

tasks.processResources {
	// The examples load their resources below "resources/".
	eachFile {
		relativePath = RelativePath(true, "resources", *relativePath.segments)
	}
	includeEmptyDirs = false
}

tasks.register<JavaExec>("run") {
	description = "Runs an example: gradlew :webrtc-examples:run -PmainClass=<class name>"
	group = "application"
	classpath = sourceSets.main.get().runtimeClasspath
	mainClass = providers.gradleProperty("mainClass")
}
