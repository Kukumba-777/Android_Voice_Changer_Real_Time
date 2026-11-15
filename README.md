# AVCRT — Android Voice Changer Real Time

System-wide real-time voice changer for rooted Android devices (KernelSU / Magisk).
Pitch & formant shifting is applied at the audio HAL level so it works for VoIP, regular
calls, voice messages, recorders — anything that opens an `AudioRecord` against the
mic / voice_communication / voice_call source.

> Status: **MVP scaffold.** All architecture, UI, persistence, root layer, IPC and the
> native audio effect skeleton are in place. The C++ effect compiles against Rubber Band
> v3 and the Android Audio Effects Framework, but has only been smoke-tested in CI; you
> should expect to tune the effect descriptor & `audio_effects.xml` overlay for your
> specific device's audio HAL.

---

## Project layout

```
avcrt/
├── build-logic/              Convention plugins (no per-module Gradle duplication)
├── gradle/libs.versions.toml The single source of truth for every version
│
├── app/                      Application module — entry, navigation, manifest, themes
│
├── core/
│   ├── common/               Coroutine dispatchers, AvcrtResult, LogSink, shared paths
│   ├── ui/                   Material 3 theme (dynamic color), design system tokens
│   ├── datastore/            DataStore<Preferences> Hilt-provided singleton
│   └── root/                 RootAccess abstraction + libsu-backed impl (KSU/Magisk/su)
│
├── domain/                   Pure business contracts: Preset, EngineState, repos, use cases
├── data/                     Repository implementations + bundled presets.json
│
├── feature/
│   ├── home/                 Master switch + status cards
│   ├── presets/              Built-in + custom presets, apply UX
│   ├── logs/                 In-memory ring buffer view with share/clear
│   └── settings/             Theme, locale, dynamic color, root, about
│
├── engine/
│   ├── api/                  Wire-format types shared with the native module
│   └── hal/                  VoiceEnginePort impl: writes JSON, installs KSU module
│
└── ksu-module/               C++ Android Audio Effect + module template + bundling task
    └── src/main/cpp/         Effect adapter, Rubber Band wrapper, JSON config reader
    └── src/main/module/      Magisk-format scripts and system overlay
```

Architecture: clean / modular MVP. Features depend only on `:domain` (the interfaces);
implementations live in `:data` and `:engine:hal` and are wired in `:app` via Hilt
multibindings. Adding a new tab is one new `:feature:*` Gradle module + one entry in
`TopDestination` — nothing else needs to change.

---

## How the runtime fits together

```
┌─────────────────────────────────────────────────────────────────────────────┐
│ APP PROCESS                                                                 │
│ ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐      │
│ │   Home VM    │  │ Presets VM   │  │   Logs VM    │  │ Settings VM  │      │
│ └──────┬───────┘  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘      │
│        │                 │                 │                 │              │
│        ▼                 ▼                 ▼                 ▼              │
│ ┌─────────────────────────────────────────────────────────────────────────┐ │
│ │           :domain — VoiceEnginePort · *Repository · UseCases            │ │
│ └────────────┬───────────────────┬──────────────────────┬─────────────────┘ │
│              │                   │                      │                   │
│      ┌───────▼──────┐    ┌──────▼──────┐         ┌─────▼─────┐              │
│      │ HalEngine    │    │  DataStore  │         │  LogSink  │              │
│      │ (engine/hal) │    │ (core/ds)   │         │ (common)  │              │
│      └───────┬──────┘    └─────────────┘         └───────────┘              │
│              │                                                              │
└──────────────┼──────────────────────────────────────────────────────────────┘
               │ writes /data/local/tmp/avcrt/active_preset.json
               │ setprop avcrt.config.revision <N>
               ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│ AUDIO HAL PROCESS  (audioserver)                                            │
│  /system/etc/audio_effects.xml routes  AUDIO_SOURCE_MIC, voice_communication│
│  voice_call, etc.  ──►  libavcrtfx.so                                       │
│                         ├ ConfigStore  ── polls system property             │
│                         └ Engine       ── Rubber Band pitch + formant shift │
└─────────────────────────────────────────────────────────────────────────────┘
```

The native effect is registered system-wide via the audio_effects manifest; once mounted
by KernelSU / Magisk and the device is rebooted, every recording app on the device routes
its mic audio through us.

---

## Building

### Prerequisites
- Android Studio Iguana or newer (or `Gradle 8.10.x` + JDK 17 + Android SDK 34)
- Android NDK 26 or newer with CMake 3.22+
- Internet access on the first build (`FetchContent` pulls Rubber Band & nlohmann/json)
- A rooted device with **KernelSU** (Magisk works too) and **Android 12+**

### Generating the Gradle wrapper

The wrapper JAR is intentionally not committed. From the project root:

```sh
gradle wrapper --gradle-version 8.10.2
```

(Android Studio will offer to do this for you on first sync.)

### Build the app

```sh
./gradlew :app:assembleDebug
```

The first build will:
1. Compile `libavcrtfx.so` for arm64-v8a / armeabi-v7a / x86_64
2. Assemble the KSU module zip from `ksu-module/src/main/module/*` + the .so files
3. Drop it into `app/src/main/assets/avcrt-module.zip` (sha256 logged)
4. Compile and package the APK

The APK ends up at `app/build/outputs/apk/debug/app-debug.apk`.

---

## Installing the audio HAL module

1. Install the APK on a rooted device.
2. Open AVCRT — the Home tab will show **“Install module”**.
3. Tap it. The app:
   - extracts `avcrt-module.zip` from its assets,
   - shells out as root (`ksud module install` / `magisk --install-module`),
   - writes a manifest under `/data/local/tmp/avcrt/`.
4. Reboot the device. The audio HAL picks up our `audio_effects.xml` overlay during boot,
   loads `libavcrtfx.so`, and registers the AVCRT effect on mic / voice_communication /
   voice_call / voice_recognition / unprocessed sources.
5. Pick a preset on the Presets tab, flip the master switch on the Home tab, then test by
   recording a voice memo or making a VoIP call.

If the install fails or the effect doesn't show up after reboot, check the Logs tab —
the install script's `ui_print` output and the native effect's logcat go through
different sinks (the in-app log is for the Kotlin side only; for native logs run
`adb logcat -s avcrtfx`).

---

## Customizing

| What | Where | How |
| --- | --- | --- |
| App version | `gradle/libs.versions.toml` (`versionCode`, `versionName`) | Bump and rebuild |
| Module version | `gradle/libs.versions.toml` (`moduleVersion`, `moduleVersionCode`) | Bump and rebuild — module.prop and `BundledModuleMeta` are regenerated |
| Built-in presets | `data/src/main/assets/presets.json` | Add an entry; both EN/RU names |
| Theme palette fallback | `core/ui/.../BrandColors.kt` | Edit the seed constants |
| Supported locales | `app/build.gradle.kts` `resourceConfigurations` + each feature's `values-XX/` | Add language tag + translations |
| Default brand color in launcher | `app/src/main/res/values/colors.xml` `launcher_background` | Single source |
| Default audio effect routing | `ksu-module/src/main/module/system/etc/audio_effects.xml` | Add/remove `<stream type="...">` blocks |

There is intentionally **no hardcoding** in the Kotlin source — preset values come from
JSON, version strings from the catalog, paths from a single constants object that the
Kotlin side and the C++ side share (`AvcrtPaths.kt` + `avcrt_paths.h`).

---

## Notes & caveats

- **License**: Rubber Band v3 is GPL, which extends to the bundled `libavcrtfx.so`. The
  Kotlin app and Hilt graph have no Rubber Band dependency and remain Apache-2.0.
- **Per-app locale**: handled via `AppCompatDelegate.setApplicationLocales` — works on
  API 31+ without needing API 33's per-app locale system.
- **Material You**: dynamic colors via `dynamicLightColorScheme` / `dynamicDarkColorScheme`.
  Falls back to `BrandColors` when the user disables dynamic color in Settings.
- **Devices that don't auto-merge overlays**: some OEM audio HALs don't honor
  `audio_effects*.xml` overlays placed under `/system/etc`. The install script falls back
  to the legacy `.conf` format when XML isn't found. Custom-AOSP devices may need a
  manual `vendor/etc` overlay — open the Logs tab for hints.
