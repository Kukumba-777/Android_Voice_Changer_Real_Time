# AVCRT user guide

[Back to the project overview](../README.md)

## Install and update

1. Install AVCRT on a rooted ARM64 device with Android 11 or newer. Give the app
   root access through KernelSU or Magisk.
2. Choose **Install module** and reboot. The regular module includes app and
   cellular audio backends. A compatible Zygisk loader is needed for the
   per-app hooks. On KernelSU this can be a separate provider, such as Zygisk Next.
3. Select a preset and turn on **System voice changer**. Open a new recording or
   call to check the result. A new session may be needed for a switch or preset
   change to take effect, depending on the capture path.

The **Cellular only** module omits both Zygisk and shared AudioFlinger capture.
Use it when you only need SIM-call processing, such as when your setup cannot
load the regular module with Zygisk. It still requires a supported call route
and matching kernel patches. It does not add processing to recordings or VoIP.

Updating the APK does not update the installed root module. Install the module
from the new APK and reboot. To keep app data when updating the APK, use the same
package name and signing key; debug and release are separate packages.

## Presets and soundboard

- **Original voice** leaves the microphone signal unmodified by the voice DSP
  while still allowing soundboard playback. It is not the same as turning AVCRT
  off: on a SIM call it can still use AVCRT's uplink, so audio levels may differ
  from a stock call.
- Import clips in **Files → Sounds**. Supported formats depend on Android's
  decoders; common examples include MP3, WAV, M4A/AAC, FLAC, and Ogg.
- Enable **Floating window** and grant the overlay permission. Start a supported
  recording or call, then use **Sounds** to play or stop a clip. Playback is not
  queued for a future capture session. It mixes one clip into the microphone
  stream at a time, and more than one compatible capture client may hear it.
- **Clip volume** adjusts the clip independently of the microphone. Imported
  clips are prepared in the background. If a large clip is not ready yet, leave
  AVCRT open until preparation finishes. Prepared audio takes about 5.5 MiB of
  storage per minute, with more space needed during preparation.

## Back up and restore

Before uninstalling, use **Files → ⋮ → Export backup** and save the ZIP outside
the app, for example in Downloads. Use **Restore backup** after reinstalling.
Backups include custom presets, imported sounds and models, and selected
preferences. Existing items with matching IDs or names are kept. Prepared clip
copies, the active preset, engine state, permissions, and the root module are
not restored.

Backup ZIPs support up to 4 GiB of uncompressed content, with a 1 GiB limit per
model. These are backup limits, not soundboard playback limits. Restoration
needs space for both the ZIP and the extracted files.

## Troubleshooting

| Symptom | What to check |
| --- | --- |
| Module installed, but the service is not running | Reboot after installation. Check that AVCRT is enabled in the root manager and look at the manager's boot log. Check the Zygisk loader for regular app capture. If KernelSU is not mounting the module's system files, a compatible mounting metamodule such as [Mountify](https://github.com/backslashxx/mountify) may help on kernels with OverlayFS. It is not required if mounting already works. |
| One recorder works but another app does not | Start a new capture after enabling AVCRT. Apps can use different capture APIs, formats, or an external recording service. A working recording is not proof that the same app's VoIP route is supported. |
| Recordings work, but SIM calls do not | SIM calls use a separate backend. Check the [supported routes and kernel requirements](architecture.md#cellular-calls). Detection of one kernel control is not proof that the entire call route is compatible. |
| A soundboard clip is missing or cuts out | Check that the clip is prepared, AVCRT is on, and a supported capture is active. If only non-speech audio drops out in a VoIP app, that app's noise suppression may be filtering it. |
| Floating controls disappear | Confirm the overlay permission. Android can hide overlays on protected screens; AVCRT does not override this. |
| Call audio becomes unstable | End the call and turn AVCRT off. If audio does not recover, disable the module in the root manager and reboot. Avoid relying on an experimental route for important calls. |

To report a problem, use **Logs → Run diagnostics** and include the device,
Android build, root manager, module version, affected app or call route, and
whether the microphone was built in or connected by a headset. Review logs for
private information before sharing them.
