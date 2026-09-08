# Build Notes

To build the native code, be sure to install the prerequisite software (follow the links):

**Note**: You don't have to install the Depot Tools, the build script will do that for you.

| OS        | Build Instructions                                                |
| --------- | ----------------------------------------------------------------- |
| Linux     | [Ubuntu][build-linux-ubuntu], [other distros][build-linux-other]  |
| macOS     | Xcode 9 or higher                                                 |
| Windows   | [Visual Studio][build-windows]                                    |

Assuming you have all the prerequisites installed for your OS, run:

```shell
./gradlew build
```

On the first run, the WebRTC source tree will be loaded into the `/<user home>/webrtc` directory. This will take a while and require about 20 GB of disk space.

To use the build from another project on the same machine, publish it to the local Maven repository:

```shell
./gradlew publishToMavenLocal
```

## Build Parameters

Build parameters are passed as Gradle project properties, for example `./gradlew build -Pwebrtc.branch=branch-heads/8037`.

| Parameter          | Description                                            | Default Value               |
| ------------------ | ------------------------------------------------------ |-----------------------------|
| webrtc.branch      | The WebRTC branch to checkout.                         | branch-heads/7977           |
| webrtc.src.dir     | The absolute checkout path for the WebRTC source tree. | /\<user_home\>/webrtc       |
| webrtc.install.dir | The install path for the compiled WebRTC library. Is also used to link against a pre-compiled WebRTC library to reduce build time. | /\<user_home\>/webrtc/build |
| webrtc.platform    | The platform to build the native library for, as `<os>-<arch>`: `windows-x86_64`, `windows-aarch64`, `linux-x86_64`, `linux-aarch32`, `linux-aarch64`, `macos-x86_64` or `macos-aarch64`. Cross compiling requires the toolchain the build workflow installs. | The host platform |
| cmake.build.type   | The CMake build type.                                  | Release                     |
| natives.dir        | A directory with prebuilt native library jars, `webrtc-java-<version>-<classifier>.jar`, used instead of building the native library. The tests run against the jar of the build platform. | -                           |

[build-linux-ubuntu]: https://chromium.googlesource.com/chromium/src/+/master/docs/linux/build_instructions.md#system-requirements
[build-linux-other]: https://chromium.googlesource.com/chromium/src/+/master/docs/linux/build_instructions.md#Notes-for-other-distros
[build-windows]: https://chromium.googlesource.com/chromium/src/+/master/docs/windows_build_instructions.md#visual-studio
