package dev.avcrt.core.common

/**
 * On-device paths shared between the Android app and the KSU native module.
 *
 * Both sides MUST agree on these strings — they live here so neither side
 * hardcodes literals. The KSU module reads the matching C++ constants in
 * `avcrt_paths.h` (kept in sync manually; see README).
 */
object AvcrtPaths {
    /** Root directory the module reads at runtime. World-readable. */
    const val MODULE_DATA_DIR = "/data/local/tmp/avcrt"

    /** JSON file with the active preset’s DSP settings. */
    const val ACTIVE_PRESET_FILE = "$MODULE_DATA_DIR/active_preset.json"

    /** Sentinel file: presence + content = engine on/off. */
    const val ENGINE_STATE_FILE = "$MODULE_DATA_DIR/engine_state"

    /** Module manifest written during install — used for version diff checks. */
    const val MODULE_MANIFEST_FILE = "$MODULE_DATA_DIR/module.manifest"

    /** Boot ID at install time. A legacy empty marker is not proof of a pending reboot. */
    const val INSTALL_PENDING_MARKER = "/data/adb/avcrt_install_pending"
    const val EARLY_BOOT_FILE = "/data/adb/avcrt_early_boot"
    const val SERVICE_BOOT_FILE = "/data/adb/avcrt_service_boot"

    /**
     * Log file the daemon's supervisor (`service.sh`) appends to (avcrtd writes
     * to stderr → redirected here). Root-only; surfaced read-only in the Logs
     * tab. MUST match the `LOG=` path in ksu-module/src/main/module/service.sh.
     */
    const val DAEMON_LOG_FILE = "/data/adb/avcrt_daemon.log"
    const val CURRENT_DAEMON_LOG_GUARD =
        "[ -s '$SERVICE_BOOT_FILE' ] && [ \"\$(cat '$SERVICE_BOOT_FILE')\" = \"\$(cat /proc/sys/kernel/random/boot_id)\" ]"

    /** Module ID — also the directory name under /data/adb/modules. */
    const val MODULE_ID = BuildConfig.MODULE_ID
    const val MODULE_DIR = "/data/adb/modules/$MODULE_ID"
    const val STAGED_MODULE_DIR = "/data/adb/modules_update/$MODULE_ID"

    /** System property the native side watches to reload its config. */
    const val RELOAD_PROPERTY = "avcrt.config.revision"

    /**
     * Live DSP config mirrored to system properties. App processes cannot read the
     * preset JSON (SELinux), so the Zygisk app-capture hook reads these instead;
     * they are set (as root) alongside [ACTIVE_PRESET_FILE]. Names match the C++
     * Zygisk module (zygisk_avcrt.cpp).
     */
    const val PROP_ENABLED = "avcrt.dsp.enabled"
    const val PROP_PITCH = "avcrt.dsp.pitch"
    const val PROP_FORMANT = "avcrt.dsp.formant"
    const val PROP_GAIN = "avcrt.dsp.gain"
    const val PROP_PRESERVE_FORMANTS = "avcrt.dsp.preserve_formants"
    const val PROP_CRISPNESS = "avcrt.dsp.crispness"

    /**
     * Phase-6 effect chain, delivered to the Zygisk hook as GROUPED CSV props (one per category)
     * because PROP_VALUE_MAX is 92 bytes — a flat CSV of all params doesn't fit one prop. Each
     * prop's field ORDER is the contract shared with the native parser
     * (ksu-module/.../zygisk_avcrt.cpp parse_fx_props). The daemon instead reads the full JSON.
     */
    const val PROP_FX_GATE = "avcrt.dsp.fxgate"     // en,thresholdDb,attackMs,releaseMs
    const val PROP_FX_EQ = "avcrt.dsp.fxeq"         // en,lowDb,midDb,highDb,telephoneEn
    const val PROP_FX_MOD = "avcrt.dsp.fxmod"       // ringEn,carrierHz,ringMix,tremEn,tremRate,tremDepth,tremShape
    const val PROP_FX_DRIVE = "avcrt.dsp.fxdrive"   // distEn,drive,distMix
    const val PROP_FX_TIME = "avcrt.dsp.fxtime"     // echoEn,timeMs,feedback,mix
    const val PROP_FX_VERB = "avcrt.dsp.fxverb"     // verbEn,roomSize,damping,wet,width

    // Tier-2 grouped CSV props (same contract; names < 32 chars, each value < PROP_VALUE_MAX).
    const val PROP_FX_FILTER = "avcrt.dsp.fxfilter"   // filterEn,hpHz,lpHz,q
    const val PROP_FX_MOD2 = "avcrt.dsp.fxmod2"       // vibEn,vibRate,vibDepth,chorEn,chorRate,chorDepth,chorMix
    const val PROP_FX_FLANGER = "avcrt.dsp.fxflanger" // flanEn,flanRate,flanDepth,flanFb,flanMix
    const val PROP_FX_PHASER = "avcrt.dsp.fxphaser"   // phasEn,phasRate,phasDepth,phasFb,phasMix
    const val PROP_FX_WAH = "avcrt.dsp.fxwah"         // wahEn,sensitivity,baseHz,rangeHz,mix
    const val PROP_FX_CRUSH = "avcrt.dsp.fxcrush"     // crushEn,bits,downsample,mix
    const val PROP_FX_COMP = "avcrt.dsp.fxcomp"       // compEn,thresholdDb,ratio,attackMs,releaseMs,makeupDb

    // Tier-3 (channel-vocoder family) grouped CSV props.
    const val PROP_FX_FORMANT = "avcrt.dsp.fxformant" // formantEn,shift
    const val PROP_FX_VOCODER = "avcrt.dsp.fxvocoder" // vocEn,carrierHz,carrierType,mix
    const val PROP_FX_WHISPER = "avcrt.dsp.fxwhisper" // whisEn,mix

    // Phase 7 experimental (FFT spectral + phase-vocoder) grouped CSV props.
    const val PROP_FX_SPECTRAL = "avcrt.dsp.fxspectral" // robotEn,whisperEn,freezeEn,blurEn,blurAmt,specMix
    const val PROP_FX_AUTOTUNE = "avcrt.dsp.fxautotune" // en,strength
    const val PROP_FX_HARMON = "avcrt.dsp.fxharmon"     // en,interval1,interval2,mix
}
