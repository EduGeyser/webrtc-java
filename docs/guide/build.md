# Build Notes

**Note**: You don't have to install the Depot Tools, the build script will do that for you.

## Prerequisites

### All platforms

| Tool              | Notes                                                                                             |
| ----------------- | ------------------------------------------------------------------------------------------------- |
| JDK 25 or newer   | The libraries are compiled for Java 25. CI builds with JDK 27. Gradle comes with the wrapper.     |
| Git               | Clones `depot_tools`, the WebRTC sources and the submodules.                                      |
| CMake 3.20 or newer | Must be on the `PATH`. The Gradle build does not download it.                                   |
| Python 3          | Runs `depot_tools` and the Linux sysroot script. On macOS it also needs `setuptools`.             |
| Meson and Ninja   | Full variant only: they build dav1d, the AV1 software decoder linked into FFmpeg.                 |
| Disk space        | About 10 GB for the WebRTC checkout, kept in `~/webrtc` by default.            |

The full variant builds FFmpeg and dav1d from Git submodules, which a plain clone leaves empty. Fetch them before the first full build:

```shell
git submodule update --init --depth 1 webrtc-media/third-party/ffmpeg webrtc-media/third-party/dav1d
```

### Linux

Linux builds run on an x86-64 host and cross-compile for ARM. CI installs these build tools (Debian/Ubuntu package names):

```shell
sudo apt-get install binutils cmake git libc++-dev libc++abi-dev locales lsb-release ninja-build pipewire pipewire-pulse pkg-config python3 python3-setuptools rsync unzip wget xz-utils
```

The full variant also needs `libva-dev meson nasm`. `libva-dev` provides the VA-API headers FFmpeg is built against, for every target architecture, cross builds included.

The JNI library compiles against the libc++ headers of the WebRTC build. Those headers only support the Clang that the pinned WebRTC commit uses, so the toolchain files look for that Clang in `/opt/clang` first. Its package is named after `CLANG_REVISION` and `CLANG_SUB_REVISION` in `clang/scripts/update.py` of the Chromium tools revision that WebRTC's `DEPS` pins, as `clang-<revision>-<sub revision>.tar.xz`. The *Install the clang of the WebRTC branch* step of `.github/actions/prepare-linux/action.yml` looks it up; install it the same way:

```shell
wget https://commondatastorage.googleapis.com/chromium-browser-clang/Linux_x64/$CLANG_PACKAGE
sudo mkdir -p /opt/clang
sudo tar -xf $CLANG_PACKAGE -C /opt/clang
```

Headers and libraries (X11, D-Bus, GIO, GBM, libdrm) come from a Debian sysroot in `/opt/sysroot` rather than from the host, so no `-dev` packages are needed for them. Install the sysroot for each target architecture you build for (`amd64`, `arm64` or `armhf`):

```shell
python3 webrtc-jni/src/main/cpp/dependencies/webrtc/linux/sysroot/install-sysroot.py --arch=amd64
sudo mkdir -p /opt/sysroot
sudo mv webrtc-jni/src/main/cpp/dependencies/webrtc/linux/debian* /opt/sysroot/
```

To cross-compile for ARM, CI also installs `gcc-aarch64-linux-gnu g++-aarch64-linux-gnu` (`-Pwebrtc.platform=linux-aarch64`) or `gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf` (`-Pwebrtc.platform=linux-aarch32`).

Running the tests of the full variant requires the runtime libraries its native library links against. A desktop system already has them. On a headless one, install them with:

```shell
sudo apt-get install libx11-6 libxext6 libxfixes3 libxdamage1 libxtst6 libxrandr2 libxcomposite1 libglib2.0-0 libgbm1 libdrm2
```

The data channels variant links nothing beyond glibc and `libgcc_s`.

### macOS

- Xcode with the macOS 15 SDK or newer, selected with `xcode-select`. CI uses Xcode 27.2. The library targets macOS 11 and later.
- CMake and the Python `setuptools` from [Homebrew](https://brew.sh), and for the full variant the tools that build FFmpeg and dav1d:

```shell
brew install cmake python-setuptools
brew install meson nasm ninja pkgconf
```

`nasm` assembles FFmpeg's x86 code, so the full variant's Intel build (`-Pwebrtc.platform=macos-x86_64`) needs it, also when you cross-compile from Apple Silicon.

### Windows

- **Visual Studio** with the *Desktop development with C++* workload. The build uses this local installation, not Google's internal toolchain. Add the *MSVC ARM64 build tools* to build for `windows-aarch64`.
- **Windows SDK 10.0.28000.0**, the version the pinned WebRTC commit expects (`SDK_VERSION` in `build/vs_toolchain.py`). Install it with the *Debugging Tools for Windows* feature, because the toolchain setup needs `dbghelp.dll`. To build with another installed SDK, pass `-Pwebrtc.windows.sdk.version`.
- **Ninja**, which CI installs with `choco install ninja`.
- For the full variant, **MSYS2** installed in `C:\msys64` (set `MSYS2_ROOT` for another location), because FFmpeg's configure scripts and makefiles need a POSIX shell with `make` and `nasm`, which Git for Windows' bash lacks, and **Meson** on the `PATH`:

  ```shell
  winget install MSYS2.MSYS2
  C:\msys64\usr\bin\bash -lc "pacman -S --needed make nasm diffutils pkgconf"
  pip install meson
  ```

CI runs Gradle from Git Bash.

## Building

With the prerequisites installed, run:

```shell
./gradlew build
```

On the first run, the WebRTC source tree is loaded into the `/<user home>/webrtc` directory, and in the full variant FFmpeg and dav1d are built into `/<user home>/ffmpeg`. This takes a long time. Later builds reuse both as long as their configuration is unchanged. The checkout carries no history; pass `-Pwebrtc.checkout.history=true` to fetch it, for example to bisect libwebrtc from this checkout. To build without running the tests, use `./gradlew assemble`, as CI does.

To use the build from another project on the same machine, publish it to the local Maven repository:

```shell
./gradlew publishToMavenLocal
```

## Build Parameters

Build parameters are passed as Gradle project properties, for example `./gradlew build -Pwebrtc.branch=branch-heads/8059`.

| Parameter          | Description                                            | Default Value               |
| ------------------ | ------------------------------------------------------ |-----------------------------|
| webrtc.branch      | The WebRTC branch or commit to check out.             | A commit on `main`, see `gradle.properties` |
| webrtc.src.dir     | The absolute checkout path for the WebRTC source tree. | /\<user_home\>/webrtc       |
| webrtc.install.dir | The install path for the compiled WebRTC library. Is also used to link against a pre-compiled WebRTC library to reduce build time. | /\<user_home\>/webrtc/build |
| webrtc.platform    | The platform to build the native library for, as `<os>-<arch>`: `windows-x86_64`, `windows-aarch64`, `linux-x86_64`, `linux-aarch32`, `linux-aarch64`, `macos-x86_64` or `macos-aarch64`. Cross compiling requires the toolchain the build workflow installs. | The host platform |
| cmake.build.type   | The CMake build type.                                  | Release                     |
| webrtc.variant      | The variant of the native library: `full`, or `data-channels` for a library without audio and video support, see below. | `data-channels`, see `gradle.properties`; `full` without it |
| webrtc.windows.sdk.version | The Windows SDK version to build WebRTC with, when the one its build scripts expect is not installed. | The version the WebRTC build scripts expect |
| webrtc.checkout.history | Whether to fetch the history of WebRTC and its dependencies. | false |
| ffmpeg.version     | The FFmpeg release the media module's submodule is pinned to. | See `gradle.properties` |
| ffmpeg.install.dir | The install path for the compiled FFmpeg libraries of the media module. A build reuses the libraries it finds there. | /\<user_home\>/ffmpeg/\<platform\> |
| natives.dir        | A directory with prebuilt native library jars, `webrtc-java-<version>-<classifier>.jar` and, for the full variant, `webrtc-java-media-<version>-<classifier>.jar`, used instead of building the native libraries. The tests run against the jars of the build platform. | -                           |

## Data Channels Only

Applications that only exchange data can build a native library without the audio and video parts of WebRTC:

```shell
./gradlew build -Pwebrtc.variant=data-channels
```

The Java API stays the same. The audio and video classes throw an `UnsatisfiedLinkError` when they are used with this library, and `PeerConnectionFactory` ignores an audio device module. The [client API](/guide/client-api) creates its runtime, peers and data channels with this library too; its audio and video calls throw a `ConfigurationException` instead. libwebrtc is built without codecs, audio processing, audio devices, cameras and desktop capture, into its own directory (`/<user home>/webrtc/build-data-channels` by default), and the resulting library is a fraction of the size of the full one.

This fork selects this variant in `gradle.properties`, and its published native libraries are this variant. Pass `-Pwebrtc.variant=full` to build the full library. The Build workflow builds and tests both variants in one run, each on runners of its own, and a dispatch can choose one of them. A release builds the variant from `gradle.properties`.

## Media Module

The full variant also builds the `webrtc-java-media` module, which plays media files and records calls with FFmpeg. It builds FFmpeg and dav1d from the `webrtc-media/third-party/ffmpeg` and `webrtc-media/third-party/dav1d` submodules, so check them out first:

```shell
git submodule update --init --depth 1 webrtc-media/third-party/ffmpeg webrtc-media/third-party/dav1d
./gradlew build -Pwebrtc.variant=full
```

Building FFmpeg needs `make` and `nasm`; on Windows they come from MSYS2, as the [Media Files](/guide/media/media-files) guide describes. The first build compiles FFmpeg into `/<user home>/ffmpeg/<platform>`, and later builds reuse it. The data channels variant has no audio and video, so it skips the module's native build, tests and publication.

## Reusing a Compiled WebRTC Install Tree

To get the Linux x86-64 headers and static libraries without compiling WebRTC locally, dispatch the Build workflow with `upload-webrtc-install` enabled. This option defaults to off. Download the `webrtc-install-linux-x86_64-<variant>-<fingerprint>` artifact and extract its tar archive into a separate directory. The artifact expires after seven days.

Pass that directory as `-Pwebrtc.install.dir=<absolute-path>` when building the JNI library. Use the same variant and native build configuration as the workflow run. The fingerprint covers the WebRTC revision, native build properties, build scripts, patches, toolchains, and build JDK version. A JAR version change alone does not change it.

## Checking JNI References

The tests can run under `-Xcheck:jni`, which validates the local and global references of every JNI call, aborts the JVM on a violation, and warns about a JNI call made while the previous one's exception was left unchecked. It is opt-in, since every JNI call gets slower:

```shell
./gradlew :webrtc:test :webrtc-media:test -Pjni-check
```

With `-Pnatives.dir`, the tests run against native library jars downloaded from a CI run instead of a local build. The platforms workflow has a `jni-check` job that runs these tests once per OS and fails on any JNI error or warning, using `.github/actions/test-natives/jni-check-report.sh`. It is switched off for now, as upstream's is.

## WebRTC patches and cached builds

The native build applies the patches in
`webrtc-jni/src/main/cpp/dependencies/webrtc/patches/common/` on every platform,
followed by the platform-specific patches. Common safety patches are required:
configuration stops if one cannot be applied.

A compiled WebRTC build records its GN arguments, extra targets, and patch
fingerprint. Changes to patch content invalidate both build-tree and installed
caches, so a native rebuild also includes the updated patches.

## Release Workflow

Maintainers start **Prepare release** (`release.yml`) from `main`, with the next development version, such as `0.22.0-sm.1-SNAPSHOT`, and optionally the version to release, such as `0.21.0-sm.2`. Without one, the release takes the version of `gradle.properties` without `-SNAPSHOT`. Preparation commits the changelog and the version changes, and pushes `main` and the release tag in one atomic push. It then starts **Publish release** (`release-publish.yml`) on that tag. A tag that `GITHUB_TOKEN` pushes starts no workflow on its own, but `GITHUB_TOKEN` can dispatch one, so no other token is needed.

Publication requires a `vX.Y.Z-sm.N` tag whose commit is on `main` and whose version matches `gradle.properties`. Every build, test and publication job checks out the commit of the tag. After the tests pass on every platform, the workflow publishes one Maven Central bundle and creates a draft GitHub release.

A **dryRun** of Prepare release builds and tests the dispatched commit with the release version, then signs and assembles the deployment bundle and uploads it as `deployment-bundle-dry-run`. It pushes no commits or tags, starts no publication, and uses placeholder Central credentials. A dry run can also check a branch other than `main`.

The Java jars are reproducible: Gradle writes their entries in a fixed order with fixed times, and the manifest's `Build-Date` is the time of the commit rather than of the build, so the same commit builds into the same Java jars. The native library jars are not covered, since CMake and the compilers do not build byte for byte the same libraries.

If preparation pushed the tag but did not start the publication, start Publish release by hand on that tag. Before you retry a failed publication, check whether Maven Central already accepted the version, because a published release cannot be replaced. A release tag that a maintainer pushes also starts Publish release.
