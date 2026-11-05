import java.time.Duration

plugins {
	`java-library`
	`maven-publish`
	signing
	alias(libs.plugins.nmcp)
}

evaluationDependsOn(":webrtc-jni")
evaluationDependsOn(":webrtc")

val platformClassifier = rootProject.extra["platformClassifier"] as String
val buildDate = rootProject.extra["buildDate"] as String
val nativesDir = rootProject.extra["nativesDir"] as File?
val dataChannelsOnly = rootProject.extra["dataChannelsOnly"] as Boolean

description = "Media extension for webrtc-java. Reads and decodes media files and network " +
		"streams with FFmpeg and feeds them into a peer connection."

val moduleName = "io.github.sendablemetatype.webrtc.media"
val coreModuleName = "io.github.sendablemetatype.webrtc"
val nativeClassifiers = listOf(
	"windows-x86_64", "windows-aarch64",
	"linux-x86_64", "linux-aarch64", "linux-aarch32",
	"macos-x86_64", "macos-aarch64",
)

val userHome: String = System.getProperty("user.home")
val ffmpegVersion = providers.gradleProperty("ffmpeg.version")
// A build reuses an FFmpeg install it finds here instead of building again.
val ffmpegInstallDir = providers.gradleProperty("ffmpeg.install.dir").orElse("$userHome/ffmpeg/$platformClassifier")
// The Linux build takes the libc++ that WebRTC bundles from where webrtc-jni
// installed WebRTC. The module is built with the full variant only.
val webrtcInstallDir = providers.gradleProperty("webrtc.install.dir").orElse("$userHome/webrtc/build")
val cmakeBuildType = providers.gradleProperty("cmake.build.type").orElse("Release")

// The toolchain files of webrtc-jni, so that both native libraries of a
// platform are built for the same target against the same sysroot. Windows on
// x86-64 and macOS on arm64 build for the host with its own compiler.
val toolchainFiles = mapOf(
	"windows-aarch64" to "aarch64-windows-clang.cmake",
	"linux-x86_64" to "x86_64-linux-clang.cmake",
	"linux-aarch32" to "aarch32-linux-clang.cmake",
	"linux-aarch64" to "aarch64-linux-clang.cmake",
	"macos-x86_64" to "x86_64-macos-cross.cmake",
)
val toolchainFile = toolchainFiles[platformClassifier]?.let {
	project(":webrtc-jni").layout.projectDirectory.file("src/main/cpp/toolchain/$it").asFile.absolutePath
}

base {
	archivesName = "webrtc-java-media"
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
		testRuntimeOnly(files(nativesDir.resolve("webrtc-java-media-${project.version}-$platformClassifier.jar")))
	}
	else {
		testRuntimeOnly(project(path = ":webrtc-jni", configuration = "natives"))
	}
}

tasks.withType<JavaCompile>().configureEach {
	options.encoding = "UTF-8"
	options.release = 25
}

val cmakeSourceDir = layout.projectDirectory.dir("src/main/cpp")
val cmakeBuildDir = layout.buildDirectory.dir("cmake/$platformClassifier/build")
// The CMake build collects the native library, the FFmpeg libraries and their
// licenses next to its build directory.
val nativesStagingDir = layout.buildDirectory.dir("cmake/$platformClassifier/natives")

val cmakeGenerate = tasks.register<Exec>("cmakeGenerate") {
	description = "Configures the native build. The first run compiles FFmpeg, which takes a while."
	group = "build"
	// On Linux the native build takes libc++ from the WebRTC install, which
	// the JNI build installs.
	dependsOn(":webrtc-jni:cmakeBuild")

	inputs.files(fileTree(cmakeSourceDir)).withPropertyName("sources")
	inputs.property("platform", platformClassifier)
	inputs.property("ffmpegVersion", ffmpegVersion)
	inputs.property("ffmpegInstallDir", ffmpegInstallDir)
	inputs.property("webrtcInstallDir", webrtcInstallDir)
	inputs.property("buildType", cmakeBuildType)
	outputs.dir(cmakeBuildDir)

	executable = "cmake"

	if (platformClassifier.startsWith("windows")) {
		args("-A", if (platformClassifier == "windows-aarch64") "ARM64" else "x64")
	}

	args("-S", cmakeSourceDir.asFile.absolutePath)
	args("-B", cmakeBuildDir.get().asFile.absolutePath)
	if (toolchainFile != null) {
		args("-DMEDIA_TOOLCHAIN_FILE=$toolchainFile")
	}
	args("-DWEBRTC_INSTALL_DIR=${webrtcInstallDir.get()}")
	args("-DFFMPEG_INSTALL_DIR=${ffmpegInstallDir.get()}")
	args("-DFFMPEG_VERSION=${ffmpegVersion.get()}")
	args("-DCMAKE_BUILD_TYPE=${cmakeBuildType.get()}")
	args("-DOUTPUT_NAME_SUFFIX=$platformClassifier")
}

val cmakeBuild = tasks.register<Exec>("cmakeBuild") {
	description = "Compiles the native library and collects it with the FFmpeg libraries."
	group = "build"
	dependsOn(cmakeGenerate)

	inputs.files(fileTree(cmakeSourceDir)).withPropertyName("sources")
	inputs.property("platform", platformClassifier)
	inputs.property("buildType", cmakeBuildType)
	// Reconfiguring CMake must also invalidate the collected natives.
	inputs.property("configuration", cmakeGenerate.map { it.inputs.properties })
	outputs.dir(nativesStagingDir)

	executable = "cmake"
	args("--build", cmakeBuildDir.get().asFile.absolutePath)
	args("--config", cmakeBuildType.get())
	// A job per processor: without a number, Makefile builds start every
	// compile at once, which exhausts the memory of a small machine.
	args("--parallel", Runtime.getRuntime().availableProcessors().toString())
}

val nativeJar = tasks.register<Jar>("nativeJar") {
	description = "Packages the native libraries, the jar published with the platform classifier."
	group = "build"
	dependsOn(cmakeBuild)

	archiveBaseName = "webrtc-java-media"
	archiveClassifier = platformClassifier

	from(nativesStagingDir)

	manifest {
		attributes(
			"Automatic-Module-Name" to "$moduleName.natives." + platformClassifier.replace('-', '.'),
			"Version" to project.version,
			"Implementation-Version" to project.version,
			"Build-Date" to buildDate,
			"FFmpeg-Version" to ffmpegVersion.get(),
			"FFmpeg-License" to "LGPL-2.1-or-later",
		)
	}
}

// The module needs the audio and video of the full variant. Prebuilt native
// library jars replace the native build entirely.
listOf(cmakeGenerate, cmakeBuild, nativeJar).forEach { task ->
	task.configure {
		onlyIf("the full variant is built") { !dataChannelsOnly }
		onlyIf("no natives.dir is given") { nativesDir == null }
	}
}

tasks.assemble {
	dependsOn(nativeJar)
}

// The native library jar, for the examples.
val natives = configurations.create("natives") {
	isCanBeConsumed = true
	isCanBeResolved = false
}

artifacts {
	add(natives.name, nativeJar)
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
	title = "webrtc-java-media ${project.version} API"
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

	onlyIf("the full variant is built") { !dataChannelsOnly }

	// The FFmpeg release the native libraries have to be built from, so that
	// a test catches an install left over from an earlier one.
	systemProperty("test.ffmpeg.version", ffmpegVersion.get())

	// Opt-in check of every JNI call's references, see docs/guide/build.md.
	if (providers.gradleProperty("jni-check").isPresent) {
		jvmArgs("-Xcheck:jni")
	}
	dependsOn(tasks.jar, project(":webrtc").tasks.named("jar"))
	if (nativesDir == null) {
		dependsOn(cmakeBuild)
	}

	// A hung test JVM fails the build instead of blocking a CI runner.
	timeout = Duration.ofMinutes(30)

	testLogging {
		events("passed", "skipped", "failed")
	}

	// The tests run inside the module, as in the webrtc project: both module
	// jars on the module path, the test classes patched into this module, the
	// native libraries and JUnit on the class path.
	val coreBuildDir = project(":webrtc").layout.buildDirectory.get().asFile
	classpath = configurations.testRuntimeClasspath.get().filter { !it.startsWith(coreBuildDir) }
	if (nativesDir == null) {
		classpath += files(nativesStagingDir)
	}

	val moduleJar = tasks.jar.flatMap { it.archiveFile }
	val testOutput = sourceSets.test.map { it.output }
	// The patched module includes the test resources, the media files the
	// tests play, which the replaced class path no longer brings along.
	inputs.files(testOutput).withPropertyName("testOutput")
	val coreModuleJar = project(":webrtc").tasks.named<Jar>("jar").flatMap { it.archiveFile }

	jvmArgumentProviders.add(CommandLineArgumentProvider {
		val patch = testOutput.get().filter { it.exists() }.asPath
		val opens = listOf(
			"io.github.sendablemetatype.webrtc.media.player",
			"io.github.sendablemetatype.webrtc.media.recorder",
		)

		listOf(
			"--module-path", listOf(moduleJar.get().asFile, coreModuleJar.get().asFile).joinToString(File.pathSeparator),
			"--add-modules", moduleName,
			"--patch-module", "$moduleName=$patch",
			"--add-reads", "$moduleName=ALL-UNNAMED",
			"--enable-native-access=$coreModuleName,$moduleName",
		) + opens.flatMap { listOf("--add-opens", "$moduleName/$it=ALL-UNNAMED") }
	})
}

// Consumers ask for the natives of their platform themselves, with a second
// dependency carrying the platform classifier: the module builds them itself,
// so a dependency on them could not be resolved on a clean machine. Gradle
// module metadata would bypass the parent pom, as in the webrtc project.
tasks.withType<GenerateModuleMetadata>().configureEach {
	enabled = false
}

val checkNatives = tasks.register("checkNatives") {
	description = "Checks that natives.dir holds the native library jar of every platform."
	val dir = nativesDir
	val version = project.version.toString()

	onlyIf("natives.dir is given") { dir != null }
	doLast {
		val missing = nativeClassifiers.filter { !dir!!.resolve("webrtc-java-media-$version-$it.jar").isFile }

		if (missing.isNotEmpty()) {
			throw GradleException("Missing media native library jars in $dir: ${missing.joinToString()}")
		}
	}
}

// Published with the full variant only, whose natives it needs.
if (!dataChannelsOnly) {
	publishing {
		publications {
			create<MavenPublication>("maven") {
				artifactId = "webrtc-java-media"

				from(components["java"])

				if (nativesDir != null) {
					// A release: the native library jars of every platform, collected
					// from the per-platform builds into one deployment.
					nativeClassifiers.forEach { nativeClassifier ->
						artifact(nativesDir.resolve("webrtc-java-media-${project.version}-$nativeClassifier.jar")) {
							classifier = nativeClassifier
							builtBy(checkNatives)
						}
					}
				}
				else {
					artifact(nativeJar) {
						classifier = platformClassifier
					}
				}

				pom {
					name = "webrtc-java-media"
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
