# Build AVCRT from source

[Back to the project overview](../README.md)

## Requirements

- JDK 17, Git, and the Android SDK. The project uses the Gradle wrapper.
- The Android SDK platform, build tools, NDK, and CMake versions specified in
  [`gradle/libs.versions.toml`](../gradle/libs.versions.toml).
- Internet access for the first build. Gradle resolves Android dependencies and
  CMake fetches SoundTouch, nlohmann/json, and the AOSP tinyalsa fork.

## Build the APK and modules

Open the repository in Android Studio and build the debug variant, or run from
the repository root:

```sh
./gradlew :app:assembleDebug
```

On Linux, if the required SDK components are not installed yet, set
`ANDROID_HOME` to your SDK directory, install the Android command-line tools,
and run [`build_avcrtd.sh`](../build_avcrtd.sh). The script uses `sdkmanager` to
install the required components and then builds the same debug APK.

```sh
./build_avcrtd.sh
```

| Output | Contents |
| --- | --- |
| `app/build/outputs/apk/debug/app-debug.apk` | Debug Android app with both module ZIPs embedded. |
| `ksu-module/build/outputs/avcrt-module.zip` | Regular root module with app-capture backends and the cellular daemon. |
| `ksu-module/build/outputs/avcrt-module-cellular.zip` | Cellular-only variant without Zygisk or shared AudioFlinger capture. |

The packaged native module is ARM64. Gradle builds it from source through the
Android NDK and CMake. App and module versions are defined in the version catalog;
the module's `module.prop` and its version metadata in the app are generated at
build time. Kernel patches for SIM calls are separate from this workflow.

## Build outputs and redistribution

APK and ZIP outputs are generated, not checked into Git. GitHub Actions checks
builds on pull requests and only uploads packages when requested in a manual
run. SoundTouch is distributed under the LGPL; preserve applicable third-party
notices and follow the dependency license terms when redistributing binaries.
