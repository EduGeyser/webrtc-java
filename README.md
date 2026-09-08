# webrtc-java, data channels only

A fork of [devopvoid/webrtc-java](https://github.com/devopvoid/webrtc-java), the Java wrapper for the [WebRTC Native API](https://webrtc.github.io/webrtc-org/native-code/native-apis), built for applications that only exchange data. The native library carries no codecs, audio processing, audio devices, cameras or desktop capture, which makes it a fraction of the size of the full one. The Java API is the one of upstream; the audio and video classes throw an `UnsatisfiedLinkError` with this library.

The fork follows upstream closely: it is upstream's `main` with the [data channels only variant](docs/guide/build.md#data-channels-only) selected, its own package and coordinates, and at times a newer WebRTC branch.

## Usage

The library is published to Maven Central as `io.github.sendablemetatype.webrtc:webrtc-java`. The native library for each platform is a separate artifact with a classifier:

- `windows-x86_64`
- `windows-aarch64`
- `linux-x86_64`
- `linux-aarch32`
- `linux-aarch64`
- `macos-x86_64`
- `macos-aarch64`

Maven resolves the classifier of the build machine automatically. Gradle does not, so declare the platforms you need:

```kotlin
implementation("io.github.sendablemetatype.webrtc:webrtc-java:VERSION")
runtimeOnly("io.github.sendablemetatype.webrtc:webrtc-java:VERSION:linux-x86_64")
```

The main module is named `io.github.sendablemetatype.webrtc`, the native library modules `io.github.sendablemetatype.webrtc.natives.<os>.<arch>`.

Versions are `<upstream version>-sm.<n>`: the upstream version the fork is based on, then the fork's own iteration.

## Building

See the [build notes](docs/guide/build.md). The variant is preselected in `gradle.properties`.

The Java packages are renamed from upstream's `dev.onvoid.webrtc` with `tools/rename-packages.py`. After merging from upstream, run it from the repository root; it moves and renames whatever arrived and leaves the rest alone.

## License

Copyright (c) 2019 Alex Andres

Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except in compliance with the License. You may obtain a copy of the License at

[http://www.apache.org/licenses/LICENSE-2.0](http://www.apache.org/licenses/LICENSE-2.0)

Unless required by applicable law or agreed to in writing, software distributed under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the License for the specific language governing permissions and limitations under the License.
