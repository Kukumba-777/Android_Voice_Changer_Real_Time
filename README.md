# AVCRT — Android Voice Changer Real Time

AVCRT is a real-time voice changer for rooted Android devices. It provides an
Android app for controlling voice presets and a root module with native audio
processing backends.

Results depend on the Android build, root setup, and the audio path used by
each app; not every recorder, camera, VoIP app, or phone call is guaranteed to
work.

## What works

- **App audio capture:** The Zygisk backend processes supported microphone
  capture in app processes, including compatible `AudioRecord` paths. Testing
  confirmed changed voice in saved Camera, Telegram, and Recorder audio.
  An app that delegates recording to a separate service or uses an unsupported
  audio format/path may not be processed.
- **GSM/cellular calls:** Voice processing in cellular calls requires a
  compatible kernel with the AVCRT patches. The daemon prefers a supported
  legacy route when available, otherwise it can try the experimental AGM
  backend on compatible Qualcomm AudioReach routes. See
  [GSM kernel patches](#gsm-kernel-patches).
- **Soundboard:** Import your own clips and play them from the floating menu.
  Clips are mixed into supported microphone recordings and calls, not played
  through the speaker. See [Floating soundboard](#floating-soundboard) for setup.

### Experimental AGM for SIM calls

When AVCRT is enabled and there is no legacy route, `avcrtd` checks the active
SIM-call route and the available kernel and vendor audio interfaces, not the
device model. The built-in microphone path needs an idle VA capture link, an
active codec TX route, a kernel mute lease, and compatible private graphs via
the vendor AGM client. The USB-C path has separate requirements below. Both
use the active preset, including **Original voice**, and the same soundboard
and effects as the other AVCRT backends.
No additional setup command, external audio source, or test signal is needed.

The worker only requests TX mute after a full, fresh processed block is
accepted by AGM and the private uplink has returned a buffer (`WRITE_DONE`).
Startup accepts quiet processed PCM without a minimum RMS; it still won't
arm on zero PCM or an incomplete Engine block.
The queue stays at four 10 ms buffers. If it fills, the worker waits at most
one period, within the current block's freshness deadline. An error or short
write ends the attempt without retrying that block. The worker releases mute
before closing its graphs and restoring the VA route. If it hangs, the kernel
lease expires; unverified cleanup blocks further AGM attempts until reboot.
Microphone mute and privacy state are sampled, not checked atomically. The
ALSA checks do not guarantee exclusivity against Sound Trigger. `WRITE_DONE`
confirms buffer return, not DSP processing status, the `Incall_Music` binding
to a VSID, or what the other party heard.

Preset changes use the already-open graphs and active TX lease. During DSP
warm-up or a brief empty VA read, the active path may submit up to 20 consecutive
zero PCM blocks instead of the stock microphone. This never arms TX at cold
start, and each zero block still needs a valid route, microphone permission,
uplink credit and full write. On an established path a timing-only miss can
drop the late block without releasing or renewing mute. The worker re-anchors
its cadence, discards queued VA and pitch output, and resumes with silence
until fresh processed PCM is ready. This is limited to three consecutive
dropped periods and a 120 ms window from the last successful renewal; it
does not apply to an invalid route, app disable, microphone privacy or I/O
failure. Persistent failures still return to stock TX.
If startup fills all four buffers without a single `WRITE_DONE`, the daemon
can rebuild the graph twice, after 400 ms and then 1200 ms from each verified
rollback. An established path can separately recover once after a transient
capture, timing or uplink credit failure. If the kernel
clears an active TX mute lease while the call, stock ALSA route and VA route
still match, the daemon can rebuild the graph after verified rollback. It
never reuses the revoked lease or renews mute while waiting. These rebuilds
are limited to four per five-second burst, with a short backoff. Each attempt
uses a fresh preflight for the same call. A closed or unknown Android
microphone state stops VA. After verified rollback, the daemon waits with
read-only checks and does not reopen VA/AGM until both microphone and
privacy checks clear. The first recheck can run immediately, with a minimum
400 ms between successive microphone-triggered worker launches. It then
rechecks call binding and app enablement,
keeps legacy priority and runs the full ALSA/VA preflight again without
requiring a preset change. A new route gets its own preflight; an unclean
worker exit still blocks AGM. Stock audio can be audible until a new graph
is ready. The daemon logs handoff timings and, per worker, aggregate
PCM levels for up to 200 processed periods on the starting preset; it does
not record audio.

A consented Poco F6 lab call confirmed processed-only audio on one handset
route. The automatic path has carried processed audio on this route, but
continuous operation and clean fallback must still be verified in live calls.
For the first test on a supported kernel, use a consenting participant and
check that their audio returns to the stock microphone when AVCRT is turned off.

The USB-C SIM-call replacement preview requires compatible vendor AGM/ACDB
graphs and an SPF kernel with the AVCRT USB uplink cutoff lease and capture
cleanup interfaces. On the tested Poco F6 setup, the SPF `0017` module provides
these interfaces. The backend uses private AGM capture and `Incall_Music`, with
no separate USB switch. The stock uplink is cut after a fresh processed buffer
completes; stock audio can still be audible during startup or after rollback.
Without the matching kernel interfaces or graph, the USB worker refuses
replacement rather than running the older mix mode. After a confirmed rollback
on an unchanged USB route, a revoked lease triggers a bounded automatic restart
with the same preset. The restart waits for a fresh microphone/privacy check
to confirm both are clear, including when the DSP revokes the lease before
Android reports mute. This recovery does not guarantee silence during the
transition.

Offline host CTest covers selection, uplink backpressure, rollback, path-reply
validation and soundboard mixing. Pass
`-DAVCRT_HOST_SOUNDTOUCH_SOURCE_DIR=/path/to/soundtouch-2.3.3`
at CMake configure time to also test the real `Engine` with **Original voice**,
soundboard mixing, app disable and DSP warm-up.

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
- Android 11 (API 30) or newer to install the app. Capture support still depends
  on the device and its audio stack.
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

GitHub Actions checks the build on pull requests and manual runs. APKs and ZIPs
are not committed. Actions only uploads them when explicitly requested in a
manual run (including requests to send the APK to Telegram).

## Install and use

1. Install the AVCRT APK on a rooted device and grant root access when prompted.
2. In the app, choose **Install module** and install the bundled module.
3. Reboot so the root manager can load the module and its Zygisk payload.
4. Select a preset, enable processing, and start a **new** recording or call.
5. Listen to the saved recording or have the other party verify the call audio.

If KernelSU installs AVCRT but does not mount its system files after reboot,
your setup may need a mounting metamodule. [Mountify](https://github.com/backslashxx/mountify)
is one option if your kernel supports OverlayFS. If AVCRT already mounts
correctly, you do not need to install Mountify.

Changing a preset or the master switch during an active recording/call may not
take effect until the next capture session. Installing a new APK does not
automatically replace a root module already installed by the manager; update
the module and reboot as needed.

### Floating soundboard

- **Files → Sounds** imports MP3, WAV, M4A/AAC, FLAC, Ogg and other formats that
  Android can decode on the device. There is no AVCRT time or size limit per
  clip. In the floating window, open **Sounds** to sort and play/stop clips.
  Playback mixes one clip into supported microphone capture after the voice DSP;
  it does not play through the device speaker.
- With the updated root module, clips prepare in the background after import or
  restore. Existing clips are also prepared after the first launch. A ready clip
  starts without decoding or copying on Play; the first play of a clip still
  being prepared will wait for it. Leave AVCRT open for large imports to finish.
- Besides the imported original, prepared clips take about 5.5 MiB of disk space
  per minute of audio, with up to twice that during preparation. Prepared copies
  are not included in backups and are removed when the sound is deleted from the
  library or the root module is uninstalled.
- **Clip volume** (10–100%, default 100%) adjusts the clip, not the microphone.
- If speech comes through but music cuts out in a WhatsApp call, turn off
  **Noise cancellation** in the WhatsApp call menu, if available. This is
  WhatsApp's own setting; there is no need to change Android's audio effects
  or AVCRT's gain to fix those music dropouts.
- Use **Original voice** with AVCRT still enabled to bypass voice DSP while keeping
  the soundboard available. In cellular calls AVCRT still uses its existing uplink
  route, so levels and device compatibility can differ from an unmodified call.
- Update the AVCRT root module from the new APK and reboot. Start a supported
  recording or call before pressing play: playback is not queued until a
  microphone opens. The soundboard follows AVCRT's capture
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
- Each ZIP supports up to 4 GiB of uncompressed content (up to 1 GiB per model).
  This is a backup limit, not a limit on soundboard imports. Restoring needs
  temporary storage for both the ZIP and the extracted files.

## GSM kernel patches

Cellular-call support is separate from app recording support. The native daemon
needs the AVCRT kernel controls and a compatible call-audio route. OnePlus Nord
(`avicii`) is the confirmed legacy device. Its kernel patches are
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
