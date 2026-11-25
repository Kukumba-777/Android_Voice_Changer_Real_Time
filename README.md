# AVCRT — Android Voice Changer Real Time

AVCRT changes the sound of your microphone in real time on rooted Android
devices. Use it to apply voice presets or mix soundboard clips into compatible
recordings and calls. The Android app controls presets, effects, and playback;
the KernelSU/Magisk module processes the audio. Processing runs on the device.

## What works

| Feature | Current support |
| --- | --- |
| **Voice presets and effects** | Built-in and custom presets with pitch, EQ, gain, and effects such as echo and reverb. **Original voice** bypasses voice effects while keeping the soundboard available. |
| **App recordings and VoIP** | Works on supported microphone paths. Processed audio has been confirmed in saved recordings from Camera, Telegram, and Recorder. That does not guarantee support for every app version or VoIP call. |
| **Soundboard** | Import clips and play them from the floating window. Audio is mixed into the microphone stream, not played through the phone speaker. |
| **Cellular (SIM) calls** | Works on specific routes with matching kernel patches. Support depends on the call audio path, not just the phone model. |
| **AI voice conversion (RVC)** | **Not functional.** The app can import ONNX models, but they do not change the live voice. Standard presets and the soundboard do not depend on RVC. |

## Confirmed devices for SIM calls

| Device | Confirmed routes | Requirements |
| --- | --- | --- |
| **OnePlus Nord 1** (`avicii`) | Legacy cellular audio route | Matching AVCRT patches for the vendor Linux 4.19 kernel. |
| **Xiaomi Poco F6** (`peridot`) | Built-in handset microphone and USB-C headset microphone on compatible routes; experimental | Matching AVCRT patches for the vendor Linux 6.1 audio modules and compatible Qualcomm AudioReach/AGM firmware. |

These are confirmed device and route combinations, not a model allowlist or a
guarantee for every firmware, accessory, or speakerphone mode. Other devices can
use the same backend if their kernel and audio stack expose the required
interfaces. Ordinary app recording does not require the SIM-call patches.
See [cellular routing and kernel patches](docs/architecture.md#cellular-calls).

> **Experimental:** Modules and kernel patches can cause audio glitches, brief
> returns to the original microphone, or loss of call audio. Keep a known-good
> boot image and a recovery plan before installing a patched kernel. Do not
> install patches or kernel images intended for a different device.

## Quick start

1. Use an ARM64 device running **Android 11 or newer** with **KernelSU or Magisk**.
2. Install the APK, grant root access, choose **Install module**, and reboot.
   For Zygisk app capture, make sure a compatible Zygisk loader is active.
3. Select a preset, turn on **System voice changer**, and start a new recording
   or supported call. For soundboard playback, import a clip in **Files → Sounds**
   and use **Sounds** in the floating window.

See the [user guide](docs/user-guide.md) for choosing a module, updating,
backing up, and troubleshooting.

## Build from source

Use JDK 17 and the Android SDK, NDK, and CMake versions specified in
[`gradle/libs.versions.toml`](gradle/libs.versions.toml). From the repository root:

```sh
./gradlew :app:assembleDebug
```

The debug APK is written to `app/build/outputs/apk/debug/app-debug.apk`. It
includes the root module ZIPs. See the [build guide](docs/build.md) for setup,
artifact locations, and the SDK setup script.

## Documentation

- [User guide](docs/user-guide.md) — installation, presets, soundboard, backups,
  and troubleshooting.
- [How AVCRT works](docs/architecture.md) — DSP, Zygisk, shared AudioFlinger
  capture, cellular routing, and kernel patches.
- [Build guide](docs/build.md) — toolchain, Gradle commands, and module packages.
