pluginManagement {
	repositories {
		gradlePluginPortal()
		mavenCentral()
	}
}

dependencyResolutionManagement {
	repositories {
		mavenCentral()
	}
}

rootProject.name = "webrtc-java"

include("webrtc-jni")
include("webrtc")
include("webrtc-examples")
