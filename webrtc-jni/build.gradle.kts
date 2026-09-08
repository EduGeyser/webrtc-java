plugins {
	base
}

val platformClassifier = rootProject.extra["platformClassifier"] as String
val buildDate = rootProject.extra["buildDate"] as String
val nativesDir = rootProject.extra["nativesDir"] as File?
val dataChannelsOnly = rootProject.extra["dataChannelsOnly"] as Boolean

val userHome: String = System.getProperty("user.home")
val webrtcBranch = providers.gradleProperty("webrtc.branch")
val webrtcSrcDir = providers.gradleProperty("webrtc.src.dir").orElse("$userHome/webrtc")
// Each variant is a different libwebrtc build and has its own install directory.
val webrtcInstallDir = providers.gradleProperty("webrtc.install.dir")
	.orElse("$userHome/webrtc/build" + if (dataChannelsOnly) "-data-channels" else "")
val cmakeBuildType = providers.gradleProperty("cmake.build.type").orElse("Release")

val toolchainFiles = mapOf(
	"windows-x86_64" to "x86_64-windows-clang.cmake",
	"windows-aarch64" to "aarch64-windows-clang.cmake",
	"linux-x86_64" to "x86_64-linux-clang.cmake",
	"linux-aarch32" to "aarch32-linux-clang.cmake",
	"linux-aarch64" to "aarch64-linux-clang.cmake",
	"macos-x86_64" to "x86_64-macos-cross.cmake",
	"macos-aarch64" to "aarch64-macos-clang.cmake",
)
val toolchainFile = toolchainFiles[platformClassifier]
	?: throw GradleException("No toolchain file for platform $platformClassifier, expected one of ${toolchainFiles.keys}")

val cmakeSourceDir = layout.projectDirectory.dir("src/main/cpp")
val cmakeBuildDir = layout.buildDirectory.dir("cmake/$platformClassifier")
val installDir = layout.buildDirectory.dir("lib")

val nativeSources = fileTree(cmakeSourceDir) {
	// Sysroots installed by the Linux build and a local WebRTC checkout.
	exclude("dependencies/webrtc/linux/debian*/**", "dependencies/webrtc/webrtc-source/**")
}

val cmakeGenerate = tasks.register<Exec>("cmakeGenerate") {
	description = "Configures the native build. The first run fetches and compiles WebRTC, which takes a while."
	group = "build"

	inputs.files(nativeSources).withPropertyName("sources")
	inputs.property("platform", platformClassifier)
	inputs.property("dataChannelsOnly", dataChannelsOnly)
	inputs.property("webrtcBranch", webrtcBranch)
	inputs.property("webrtcSrcDir", webrtcSrcDir)
	inputs.property("webrtcInstallDir", webrtcInstallDir)
	inputs.property("buildType", cmakeBuildType)
	outputs.dir(cmakeBuildDir)

	executable = "cmake"

	if (platformClassifier.startsWith("windows")) {
		args("-A", if (platformClassifier == "windows-aarch64") "ARM64" else "x64")
		args("-T", "ClangCL")
	}

	args("-S", cmakeSourceDir.asFile.absolutePath)
	args("-B", cmakeBuildDir.get().asFile.absolutePath)
	args("-DWEBRTC_TOOLCHAIN_FILE=toolchain/$toolchainFile")
	args("-DWEBRTC_BRANCH=${webrtcBranch.get()}")
	args("-DWEBRTC_SRC_DIR=${webrtcSrcDir.get()}")
	args("-DWEBRTC_INSTALL_DIR=${webrtcInstallDir.get()}")
	args("-DCMAKE_BUILD_TYPE=${cmakeBuildType.get()}")
	args("-DCMAKE_INSTALL_PREFIX=${installDir.get().asFile.absolutePath}")
	args("-DOUTPUT_NAME_SUFFIX=$platformClassifier")

	if (dataChannelsOnly) {
		args("-DWEBRTC_DATA_CHANNELS_ONLY=ON")
	}
}

val cmakeBuild = tasks.register<Exec>("cmakeBuild") {
	description = "Compiles the native library and installs it into build/lib."
	group = "build"
	dependsOn(cmakeGenerate)

	inputs.files(nativeSources).withPropertyName("sources")
	inputs.property("platform", platformClassifier)
	inputs.property("buildType", cmakeBuildType)
	outputs.dir(installDir)

	executable = "cmake"
	args("--build", cmakeBuildDir.get().asFile.absolutePath)
	args("--config", cmakeBuildType.get())
	args("--target", "install")
	args("--parallel")
}

val nativeJar = tasks.register<Jar>("nativeJar") {
	description = "Packages the native library, the jar published with the platform classifier."
	group = "build"
	dependsOn(cmakeBuild)

	archiveBaseName = "webrtc-java"
	archiveClassifier = platformClassifier
	// The base plugin alone would put the jar into build/distributions.
	destinationDirectory = layout.buildDirectory.dir("libs")

	from(installDir)

	manifest {
		attributes(
			"Automatic-Module-Name" to "io.github.sendablemetatype.webrtc.natives." + platformClassifier.replace('-', '.'),
			"Version" to project.version,
			"Build-Date" to buildDate,
		)
	}
}

tasks.assemble {
	dependsOn(nativeJar)
}

// Prebuilt native library jars replace this build entirely.
listOf(cmakeGenerate, cmakeBuild, nativeJar).forEach { task ->
	task.configure {
		onlyIf("no natives.dir is given") { nativesDir == null }
	}
}

// The native library jar, consumed by the tests and examples of the other projects.
val natives = configurations.create("natives") {
	isCanBeConsumed = true
	isCanBeResolved = false
}

artifacts {
	add(natives.name, nativeJar)
}
