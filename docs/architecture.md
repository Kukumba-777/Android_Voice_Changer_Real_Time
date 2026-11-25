# How AVCRT works

[Back to the project overview](../README.md)

AVCRT separates the controls from the audio processing. The Kotlin/Compose app
stores presets and the enabled state, manages the installed root module, and
controls the soundboard. Native C++ code in the module processes audio where the
active backend can access it. AVCRT does not create a virtual microphone device
that every app can select.

## Voice DSP and soundboard

The shared [`Engine`](../ksu-module/src/main/cpp/src/avcrt_engine.cpp) processes
PCM audio. It applies configured voice effects and uses
[SoundTouch](https://codeberg.org/soundtouch/soundtouch) for pitch and tempo
processing. The effects chain supplies features such as EQ, gating, distortion,
echo, and reverb. Formant preservation is an approximation, not an independent
formant shifter or AI voice conversion.

The app decodes and prepares imported clips ahead of playback. On a backend
that supports the soundboard, the engine mixes clip audio with the microphone
signal after voice processing. **Original voice** skips voice effects but still
allows that mix. Turning AVCRT off disables processing instead.

## App recordings and internet calls

| Backend | How it works | Limits |
| --- | --- | --- |
| **Zygisk** | Loads a native hook into compatible ARM64 app processes. Supported `AudioRecord` and AAudio paths can pass microphone buffers through the engine before the app encodes or sends them. | Depends on the Zygisk loader, Android audio ABI, and the capture API/format used by the app. |
| **Shared AudioFlinger capture** | Hooks a compatible audio-server input path and processes eligible PCM as Android's audio server reads it from the HAL. | Experimental and system-wide rather than per-app. It can affect multiple microphone consumers and does not cover paths that bypass the hooked input. |

These backends do not need the cellular kernel patches. For apps that record
through another process or use an unsupported path, having root or installing
the module is not enough to guarantee processing.

## Cellular calls

SIM calls use a vendor audio path to the modem that is separate from ordinary
app recording. The [`avcrtd`](../ksu-module/src/main/cpp/daemon) service selects
a compatible uplink based on the active call route and available kernel and
vendor interfaces. It prefers the legacy backend when that route qualifies.

| Route | Capture and uplink | What the kernel provides |
| --- | --- | --- |
| **Legacy ALSA** | Captures from a supported PCM route and sends processed audio through the vendor in-call music mixer. | A control to mute the stock modem microphone feed and, where supported, a control to rebuild the voice processing path. |
| **AudioReach / AGM, built-in microphone** | Uses a compatible VA (voice activation) capture path and submits processed PCM through a private AGM `Incall_Music` stream. | An observable call route and a time-limited LPASS codec TX mute lease. |
| **AudioReach / AGM, USB-C headset** | Uses a private, non-tunneled AGM capture graph and a private `Incall_Music` uplink. Requires compatible vendor AGM/ACDB graph definitions. | SPF interfaces to observe the call route, lease the stock USB uplink cutoff, and verify capture cleanup. |

On AGM routes, AVCRT waits for fresh processed audio and an uplink buffer
completion before requesting the stock microphone cutoff. A buffer completion
does not prove what the other caller heard. If the route or microphone privacy
state becomes unsuitable, AVCRT stops processing and attempts to restore the
stock route. It will not reopen a route after an unverified rollback. The kernel
lease is a fallback if the worker cannot release it. The original microphone
can still be audible at startup or during a route change; Android's microphone
mute and privacy controls are not intended to be bypassed.

### Kernel patches

Kernel patches expose route, mute, or recovery controls; they do not perform
voice effects themselves. Use source patches that match the device's kernel
and firmware. The [OnePlus Nord patch repository](https://github.com/Kukumba-777/avcrt-avicii-kernel-patches)
documents the legacy controls. For Poco F6, the LPASS and SPF patches target the
Linux 6.1 **vendor audio modules tree**, not the main kernel source tree. The app
and module build do not compile or install these kernel patches.

An available kernel interface does not guarantee that the vendor graph, active
route, or connected accessory is compatible. Adding a new device may require
kernel work as well as changes to the audio backend.

## Source layout

| Directory | Responsibility |
| --- | --- |
| [`app/`](../app/) and [`feature/`](../feature/) | App navigation, floating window, presets, files, settings, and logs. |
| [`engine/`](../engine/) | Android-side engine API, root module integration, and diagnostics. |
| [`ksu-module/src/main/cpp/`](../ksu-module/src/main/cpp/) | Native DSP, capture hooks, and cellular daemon. |
| [`ksu-module/src/main/module/`](../ksu-module/src/main/module/) | Root module installation and boot scripts. |
