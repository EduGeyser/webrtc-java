import java.time.Instant
import java.time.ZoneOffset
import java.time.format.DateTimeFormatter

plugins {
	`maven-publish`
	signing
	alias(libs.plugins.nmcp)
	alias(libs.plugins.nmcp.aggregation)
}

description = "Java native interface implementation based on the free, open WebRTC project. " +
		"The goal of this project is to enable development of RTC applications for desktop platforms running Java."

/**
 * The platform the native library is built for, in the form of the artifact
 * classifier: `<os>-<arch>`. Set `-Pwebrtc.platform=<classifier>` to cross
 * compile, for example `linux-aarch64`; the host platform is the default.
 */
fun hostPlatform(): String {
	val osName = System.getProperty("os.name").lowercase()
	val osArch = System.getProperty("os.arch").lowercase()

	val os = when {
		osName.startsWith("windows") -> "windows"
		osName.startsWith("mac") -> "macos"
		osName.contains("linux") -> "linux"
		else -> throw GradleException("Unsupported operating system: $osName")
	}
	val arch = when (osArch) {
		"amd64", "x86_64", "x86-64" -> "x86_64"
		"aarch64", "arm64" -> "aarch64"
		"arm", "armv7l", "aarch32" -> "aarch32"
		else -> throw GradleException("Unsupported CPU architecture: $osArch")
	}

	return "$os-$arch"
}

val platformClassifier = providers.gradleProperty("webrtc.platform").orElse(provider { hostPlatform() }).get()

// The variant of the native library: "full", or "data-channels" for a library
// without audio and video support. See docs/guide/build.md.
val variant = providers.gradleProperty("webrtc.variant").getOrElse("full")

if (variant != "full" && variant != "data-channels") {
	throw GradleException("Unknown webrtc.variant '$variant', expected 'full' or 'data-channels'")
}

// Shared by the subprojects.
extra["platformClassifier"] = platformClassifier
extra["dataChannelsOnly"] = variant == "data-channels"
// A directory holding prebuilt native library jars, "webrtc-java-<version>-<classifier>.jar",
// in place of the ones webrtc-jni builds: the tests run against the jar of the
// build platform, and the publication attaches the jars of every platform.
extra["nativesDir"] = providers.gradleProperty("natives.dir").map { file(it) }.orNull
extra["buildDate"] = DateTimeFormatter.ofPattern("yyyy-MM-dd HH:mm").withZone(ZoneOffset.UTC).format(Instant.now())

allprojects {
	pluginManager.withPlugin("signing") {
		// Releases are signed with the key from the environment; without a key
		// nothing is signed, which is what local and snapshot builds want.
		val signingKey = providers.environmentVariable("MAVEN_GPG_PRIVATE_KEY")
		val signingPassphrase = providers.environmentVariable("MAVEN_GPG_PASSPHRASE")

		if (signingKey.isPresent) {
			configure<SigningExtension> {
				useInMemoryPgpKeys(signingKey.get(), signingPassphrase.getOrElse(""))
				sign(the<PublishingExtension>().publications)
			}
		}
	}
}

/*
 * The parent pom. Its profiles set "platform.classifier" for the operating
 * system and architecture of the machine the pom is evaluated on, and the
 * webrtc-java pom depends on itself with that classifier, so Maven consumers
 * get the native library of their platform without declaring it.
 */
val platformProfiles = listOf(
	Triple("windows-x86_64", "windows", "amd64"),
	Triple("windows-aarch64", null, null),
	Triple("linux-x86_64", "linux", "amd64"),
	Triple("linux-aarch32", "linux", "aarch32"),
	Triple("linux-aarch64", "linux", "aarch64"),
	Triple("macos-x86_64", "mac", "x86_64"),
	Triple("macos-aarch64", "mac", "aarch64"),
)

publishing {
	publications {
		create<MavenPublication>("parent") {
			artifactId = "webrtc-java-parent"

			pom {
				packaging = "pom"
				name = "webrtc-java-parent"
				description = project.description
				url = "https://github.com/devopvoid/webrtc-java"

				developers {
					developer {
						id = "a.andres"
						name = "Alex Andres"
						email = "andres.alex@pm.me"
					}
				}
				licenses {
					license {
						name = "The Apache Software License, Version 2.0"
						url = "http://www.apache.org/licenses/LICENSE-2.0.txt"
						distribution = "repo"
					}
				}
				issueManagement {
					system = "GitHub"
					url = "https://github.com/devopvoid/webrtc-java/issues"
				}
				scm {
					connection = "scm:git:git://github.com/devopvoid/webrtc-java.git"
					developerConnection = "scm:git:ssh://git@github.com/devopvoid/webrtc-java.git"
					url = "https://github.com/devopvoid/webrtc-java/tree/main"
					tag = "HEAD"
				}

				withXml {
					val profiles = asNode().appendNode("profiles")

					platformProfiles.forEach { (classifier, osFamily, osArch) ->
						val profile = profiles.appendNode("profile")
						profile.appendNode("id", classifier)

						if (osFamily != null) {
							val os = profile.appendNode("activation").appendNode("os")
							os.appendNode("family", osFamily)
							os.appendNode("arch", osArch)
						}

						profile.appendNode("properties").appendNode("platform.classifier", classifier)
					}
				}
			}
		}
	}
}

dependencies {
	nmcpAggregation(project(":"))
	nmcpAggregation(project(":webrtc"))
}

nmcpAggregation {
	centralPortal {
		username = providers.environmentVariable("MAVEN_USERNAME")
		password = providers.environmentVariable("MAVEN_TOKEN")
		// The deployment waits in the portal until it is released by hand.
		publishingType = "USER_MANAGED"
		publicationName = "${project.group}:webrtc-java:${project.version}"
	}
}
