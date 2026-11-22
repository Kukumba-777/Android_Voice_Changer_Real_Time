# AVCRT — Android Voice Changer Real Time

AVCRT is a real-time voice changer for rooted Android devices. It provides an
Android app for controlling voice presets and a root module with native audio
processing backends.

The project has been tested on **POCO F6** and **OnePlus Nord**. Results depend
on the device, Android build, root setup, and the audio path used by each app;
not every recorder, camera, VoIP app, or phone call is guaranteed to work.

## What works

- **App audio capture:** The Zygisk backend processes supported microphone
  capture in app processes, including compatible `AudioRecord` paths. Testing
  confirmed changed voice in saved Camera, Telegram, and Recorder audio on
  POCO F6 and OnePlus Nord. An app that delegates recording to a separate
  service or uses an unsupported audio format/path may not be processed.
- **GSM/cellular calls:** Voice processing in cellular calls requires a
  compatible kernel with the AVCRT kernel patches. This has so far been tested
  on **OnePlus Nord only**. Support on other devices and kernels is unverified.
  The kernel changes are device/kernel-specific and are **not included in this
  repository**. Advanced users can adapt the required AVCRT kernel changes to
  their own kernel source, build and flash a matching test kernel, then report
  the results. This is not a ready-made universal kernel patch.

The module also contains a shared AudioFlinger/HAL capture backend for supported
audio servers. It is system-wide rather than per-app and may affect other
microphone consumers. Treat it as device-dependent experimental functionality;
verify saved recordings, routing, and mute behavior on your device before
relying on it.

## Requirements

- Rooted Android device with **KernelSU or Magisk**.
- Android 12 or newer for the app.
- For building: JDK 17, Android SDK, NDK and CMake. The pinned versions are in
  `gradle/libs.versions.toml`.
- Internet access for the first native build, which downloads the pinned
  SoundTouch, nlohmann/json, and tinyalsa sources.

## Build

Open the project in Android Studio, or build from the command line:

```sh
./gradlew :app:assembleDebug
```

The debug APK is written to:

```text
app/build/outputs/apk/debug/app-debug.apk
```

The APK bundles the regular root module. The build also creates a cellular-only
recovery module without the Zygisk app-capture payload. For a clean SDK setup,
`build_avcrtd.sh` can install the SDK components pinned by the project and build
the APK:

```sh
./build_avcrtd.sh
```

GitHub Actions builds the APK and module archives for pull requests and manual
runs. Generated APKs and ZIPs are build artifacts, not source files.

## Install and use

1. Install the AVCRT APK on a rooted device and grant root access when prompted.
2. In the app, choose **Install module** and install the bundled module.
3. Reboot so the root manager can load the module and its Zygisk payload.
4. Select a preset, enable processing, and start a **new** recording or call.
5. Listen to the saved recording or have the other party verify the call audio.

Changing a preset or the master switch during an active recording/call may not
take effect until the next capture session. Installing a new APK does not
automatically replace a root module already installed by the manager; update
the module and reboot as needed.

## GSM kernel patches

Cellular-call support is separate from app recording support. The native daemon
needs the AVCRT kernel controls and a compatible call-audio route. The only
confirmed device so far is OnePlus Nord; the kernel changes must match the
exact device kernel source and configuration. Kernel patch files are not
provided in this repository.

If you want to experiment on another device, adapt the required AVCRT kernel
changes to your kernel source, build and flash your own test kernel, and
validate calls cautiously. Keep a recovery path and a known-good boot image.
Do not flash a kernel built for another device. Please share the device model,
kernel source/revision, changes, and test results when reporting compatibility.

## Development notes

- App and module version values are maintained in `gradle/libs.versions.toml`.
- The DSP uses SoundTouch; retain its license notices when redistributing builds.
- Audio compatibility is determined by the actual capture and routing path, not
  simply by Android version or file extension.
- Local diagnostics and experiments are intentionally excluded from the GitHub
  source publication; the shipped app/module build does not require them.

## License

See the repository license and third-party notices. SoundTouch is distributed
under its own license; comply with its terms when redistributing AVCRT builds.
