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
  The patches for OnePlus Nord (`avicii`) are published separately; see
  [GSM kernel patches](#gsm-kernel-patches). They are not universal patches.
- **Soundboard:** Import your own clips and play them from the floating menu.
  Clips are mixed into supported microphone recordings and calls, not played
  through the speaker. See [Floating soundboard](#floating-soundboard) for setup.

The module also contains a shared AudioFlinger/HAL capture backend for supported
audio servers. It is system-wide rather than per-app and may affect other
microphone consumers. Treat it as device-dependent experimental functionality;
verify saved recordings, routing, and mute behavior on your device before
relying on it.

## AI voice conversion status

**AI voice conversion (RVC) does not currently work and is still in development.**
The **Files → AI models** tab can import `.onnx` files, but importing a model does
not enable AI voice conversion. This does not affect the real-time DSP presets
or the soundboard.

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

### Floating soundboard

- **Files → Sounds** imports audio clips up to 30 seconds in formats supported
  by Android on the device. In the floating window, open **Sounds** to sort and
  play/stop clips. Playback mixes a single clip into supported microphone capture
  after the voice DSP; it does not play through the device speaker.
- Use **Original voice** with AVCRT still enabled to bypass voice DSP while keeping
  the soundboard available. In cellular calls AVCRT still uses its existing uplink
  route, so levels and device compatibility can differ from an unmodified call.
- Update the AVCRT root module from the new APK and reboot. Start a supported
  recording or call before pressing play: clips are timed from the button press,
  not queued until a microphone opens. The soundboard follows AVCRT's capture
  scope and can be heard by multiple compatible recordings active at once.
- New presets and clips imported in AVCRT appear in the floating menu without
  restarting it. Android can hide overlays on protected screens (such as some
  Settings screens); AVCRT does not override this behavior.

### Updating or reinstalling the app

- Installing a newer APK **over** the same package with the same signing key
  keeps app data. A full uninstall removes app data, including the sound and
  model libraries. The app does not rely on Android's automatic backup.
- Before uninstalling, use **Files → ⋮ → Export backup** and save the ZIP outside
  AVCRT, for example in Downloads. After reinstalling, use **Files → ⋮ → Restore
  backup**. The ZIP includes custom presets, sounds, ONNX models and selected UI
  preferences. Existing presets and files with matching IDs/names are kept.
  Engine state, the active preset, root module and permissions are not restored.
- Each ZIP supports up to 4 GiB of uncompressed content (up to 1 GiB per model,
  32 MiB per clip). Restoring needs temporary storage for both the ZIP and the
  extracted files.

## GSM kernel patches

Cellular-call support is separate from app recording support. The native daemon
needs the AVCRT kernel controls and a compatible call-audio route. The only
confirmed device so far is OnePlus Nord (`avicii`). The kernel patches are
published in the separate [avcrt-avicii-kernel-patches](https://github.com/Kukumba-777/avcrt-avicii-kernel-patches)
repository. The patches must match the device's kernel source and configuration.

If your device uses a Qualcomm chipset and runs Linux kernel 4.x or newer,
these patches may be a starting point for adding GSM call support. Compatibility
is not guaranteed: adapt the patches to your device's kernel source, build and
flash your own test kernel, and validate calls cautiously. Keep a recovery path
and a known-good boot image.
Do not flash a kernel built for another device. Please share the device model,
kernel source/revision, changes, and test results when reporting compatibility.

## Development notes

- App and module version values are maintained in `gradle/libs.versions.toml`.
- The DSP uses SoundTouch; retain its license notices when redistributing builds.
- Audio compatibility is determined by the actual capture and routing path, not
  simply by Android version or file extension.
- Local diagnostics and experiments are intentionally excluded from the GitHub
  source publication; the shipped app/module build does not require them.
- Run the soundboard timing/resampling smoke test on Linux:

  ```sh
  c++ -std=c++17 -pthread -Iksu-module/src/main/cpp/src \
    ksu-module/tests/soundboard_test.cpp -o /tmp/avcrt-soundboard-test
  /tmp/avcrt-soundboard-test
  ```

## License

See the repository license and third-party notices. SoundTouch is distributed
under its own license; comply with its terms when redistributing AVCRT builds.
