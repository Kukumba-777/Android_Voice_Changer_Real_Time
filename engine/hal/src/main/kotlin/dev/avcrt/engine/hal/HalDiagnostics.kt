package dev.avcrt.engine.hal

import dev.avcrt.core.common.AvcrtPaths
import dev.avcrt.core.common.dispatcher.DispatcherProvider
import dev.avcrt.core.common.result.AvcrtError
import dev.avcrt.core.common.result.AvcrtResult
import dev.avcrt.core.root.RootAccess
import dev.avcrt.core.root.RootStatus
import dev.avcrt.engine.api.DiagnosticItem
import dev.avcrt.engine.api.DiagnosticReport
import dev.avcrt.engine.api.DiagnosticSeverity
import dev.avcrt.engine.api.EngineDiagnostics
import kotlinx.coroutines.withContext
import javax.inject.Inject
import javax.inject.Singleton

/**
 * Probes the device to answer "is the CURRENT voice-changer stack actually in
 * place and live?" — i.e. the kernel patch (cellular), the avcrtd daemon, and
 * the Zygisk app-capture hook. Every check is a read-only root shell out.
 *
 * NOTE: the legacy soundfx audio-HAL effect (libavcrtfx.so + audio_effects.xml
 * overlay) is DEAD and intentionally NOT probed here — the two live paths are
 * (1) kernel "AVCRT Dec Mute" + avcrtd for GSM calls, (2) the Zygisk hook for
 * apps. Both are driven by active_preset.json + the avcrt.dsp.* props, which we
 * also surface.
 *
 * Does NOT prompt for root — callers gate on root being granted first (see
 * [EngineDiagnosticsAutoRunner]); otherwise this returns [AvcrtError.RootDenied].
 */
@Singleton
class HalDiagnostics @Inject constructor(
    private val rootAccess: RootAccess,
    private val dispatchers: DispatcherProvider,
) : EngineDiagnostics {

    override suspend fun run(): AvcrtResult<DiagnosticReport> = withContext(dispatchers.io) {
        val status = rootAccess.currentStatus()
        if (status !is RootStatus.Available || !status.granted) {
            return@withContext AvcrtResult.Failure(AvcrtError.RootDenied("Diagnostics need root"))
        }

        val items = buildList {
            add(checkRootProvider())
            add(checkKernelPatch())
            add(checkDaemonRunning())
            add(checkDaemonLogTail())
            add(checkZygiskHook())
            add(checkModuleDirectory())
            add(checkModuleContents())
            add(checkInstallPendingMarker())
            add(checkDspProps())
            add(checkActivePresetJson())
            add(checkEngineStateFile())
        }
        AvcrtResult.Success(DiagnosticReport(System.currentTimeMillis(), items))
    }

    private fun checkRootProvider(): DiagnosticItem {
        return when (val s = rootAccess.currentStatus()) {
            is RootStatus.Available -> ok("Root", "Available via ${s.provider} (granted=${s.granted})")
            RootStatus.Unavailable -> fail("Root", "Not available — diagnostics will be incomplete")
            RootStatus.Unknown -> warn("Root", "Status unknown")
        }
    }

    /**
     * Cellular path lives ONLY if the kernel exposes the "AVCRT Dec Mute"
     * kcontrol. The daemon ships a `--probe-kcontrol` flag that exits 0 when it's
     * present — the cleanest device-agnostic check.
     */
    private suspend fun checkKernelPatch(): DiagnosticItem {
        val bin = "$MODULE_DIR/bin/avcrtd"
        if (!rootAccess.fileExists(bin)) {
            return warn("Kernel patch", "avcrtd binary missing — install the module first, then re-check.")
        }
        val probe = sh("'$bin' --probe-kcontrol >/dev/null 2>&1 && echo present || echo absent")
        val version = sh("cat /proc/version 2>/dev/null")
        val ourKernel = version.contains("avcrt", ignoreCase = true)
        return if (probe == "present") {
            ok(
                "Kernel patch",
                "AVCRT Dec Mute kcontrol present — cellular clean-replace available." +
                    if (ourKernel) " (/proc/version is an avcrt build)" else "",
            )
        } else {
            warn(
                "Kernel patch",
                "AVCRT Dec Mute kcontrol NOT found — GSM cellular voice-change is OFF (the app/Zygisk " +
                    "path still works). Flash the patched kernel (avcrt-kernel-release.zip).",
            )
        }
    }

    private suspend fun checkDaemonRunning(): DiagnosticItem {
        val pid = sh("pidof avcrtd 2>/dev/null")
        val lockHeld = rootAccess.fileExists(DAEMON_LOCK)
        return when {
            pid.isNotBlank() -> ok("Daemon", "avcrtd running (pid $pid)")
            else -> warn(
                "Daemon",
                "avcrtd not running" +
                    if (lockHeld) " (lock file present — starting or stuck in a call)" else
                        " — service.sh launches it at boot; reboot if the module was just installed.",
            )
        }
    }

    private suspend fun checkDaemonLogTail(): DiagnosticItem {
        val tail = sh("tail -n 8 ${AvcrtPaths.DAEMON_LOG_FILE} 2>/dev/null")
        return if (tail.isBlank()) {
            warn("Daemon log", "${AvcrtPaths.DAEMON_LOG_FILE} missing/empty — daemon hasn't logged yet.")
        } else {
            ok("Daemon log (last 8)", tail)
        }
    }

    /**
     * App voice-change loads only if the Zygisk hook .so is in the module AND
     * Zygisk is enabled in the manager.
     */
    private suspend fun checkZygiskHook(): DiagnosticItem {
        val so = "$MODULE_DIR/zygisk/arm64-v8a.so"
        if (!rootAccess.fileExists(so)) {
            return fail(
                "Zygisk hook",
                "zygisk/arm64-v8a.so MISSING in the module — app voice-change won't load. Reinstall the module.",
            )
        }
        val setting = sh("magisk --sqlite \"SELECT value FROM settings WHERE key='zygisk'\" 2>/dev/null")
        return when {
            setting.contains("=1") -> ok("Zygisk hook", "hook .so present + Zygisk enabled ($setting)")
            setting.isBlank() -> warn(
                "Zygisk hook",
                "hook .so present; couldn't read the Zygisk setting (magisk sqlite). " +
                    "On KernelSU, Zygisk comes from the ZygiskNext module instead.",
            )
            else -> warn(
                "Zygisk hook",
                "hook .so present but Zygisk appears DISABLED ($setting) — enable Zygisk + reboot " +
                    "for app voice-change.",
            )
        }
    }

    private suspend fun checkModuleDirectory(): DiagnosticItem {
        val activeProp = "$MODULE_DIR/module.prop"
        val pendingProp = "/data/adb/modules_update/${AvcrtPaths.MODULE_ID}/module.prop"
        val active = rootAccess.fileExists(activeProp)
        val pending = rootAccess.fileExists(pendingProp)
        val disabled = rootAccess.fileExists("$MODULE_DIR/disable")
        return when {
            active && disabled -> warn("Module dir", "$MODULE_DIR present but DISABLED (disable file) — re-enable it.")
            active -> ok("Module dir", "$MODULE_DIR — module.prop present")
            pending -> warn("Module dir", "Staged in modules_update — reboot to activate.")
            else -> fail("Module dir", "Module not installed (no module.prop in modules or modules_update).")
        }
    }

    /** Confirms the CURRENT artifacts are inside the module (daemon + hook), not the dead soundfx .so. */
    private suspend fun checkModuleContents(): DiagnosticItem {
        val listing = sh("find '$MODULE_DIR' -maxdepth 3 -type f 2>/dev/null | sort")
        val hasDaemon = listing.contains("/bin/avcrtd")
        val hasZygisk = listing.contains("/zygisk/arm64-v8a.so")
        val truncated = listing.lineSequence().take(30).joinToString("\n")
        return when {
            !hasDaemon && !hasZygisk -> fail(
                "Module contents",
                "Neither bin/avcrtd nor zygisk/arm64-v8a.so is inside the module — the bundled zip was " +
                    "empty or staging failed. Rebuild the APK from a clean state.\n--- listing ---\n$truncated",
            )
            !hasDaemon -> warn(
                "Module contents",
                "zygisk hook present but bin/avcrtd MISSING — the cellular daemon can't run." +
                    "\n--- listing ---\n$truncated",
            )
            !hasZygisk -> warn(
                "Module contents",
                "avcrtd present but zygisk/arm64-v8a.so MISSING — app voice-change won't load." +
                    "\n--- listing ---\n$truncated",
            )
            else -> ok(
                "Module contents",
                "bin/avcrtd + zygisk/arm64-v8a.so present.\n--- listing ---\n$truncated",
            )
        }
    }

    private suspend fun checkInstallPendingMarker(): DiagnosticItem {
        return if (rootAccess.fileExists(AvcrtPaths.INSTALL_PENDING_MARKER)) {
            warn("Install marker", "${AvcrtPaths.INSTALL_PENDING_MARKER} present → reboot pending")
        } else {
            ok("Install marker", "Cleared (no reboot pending)")
        }
    }

    /** The live DSP config both runtime paths read (props are app-readable; the JSON is not). */
    private suspend fun checkDspProps(): DiagnosticItem {
        // Plain loop, not joinToString { } — its `transform` is a nullable (hence
        // non-inlined) lambda, so suspend `sh()` can't be called inside it.
        val keys = listOf(
            AvcrtPaths.PROP_ENABLED,
            AvcrtPaths.PROP_PITCH,
            AvcrtPaths.PROP_FORMANT,
            AvcrtPaths.PROP_GAIN,
            AvcrtPaths.PROP_PRESERVE_FORMANTS,
            AvcrtPaths.RELOAD_PROPERTY,
        )
        val sb = StringBuilder()
        for (key in keys) {
            val value = sh("getprop $key").ifBlank { "(unset)" }
            sb.append(key).append('=').append(value).append('\n')
        }
        return ok("DSP props", sb.toString().trimEnd())
    }

    private suspend fun checkActivePresetJson(): DiagnosticItem {
        if (!rootAccess.fileExists(AvcrtPaths.ACTIVE_PRESET_FILE)) {
            return warn(
                "active_preset.json",
                "Missing — post-fs-data.sh seeds this on boot; did the module run?",
            )
        }
        val content = sh("cat '${AvcrtPaths.ACTIVE_PRESET_FILE}' 2>/dev/null")
        return ok("active_preset.json", content.take(400))
    }

    private suspend fun checkEngineStateFile(): DiagnosticItem {
        val content = sh("cat '${AvcrtPaths.ENGINE_STATE_FILE}' 2>/dev/null")
        return if (content.isBlank()) warn("engine_state", "File missing or empty") else ok("engine_state", content)
    }

    private suspend fun sh(command: String): String =
        (rootAccess.exec(command) as? AvcrtResult.Success)?.value?.trim().orEmpty()

    private fun ok(label: String, detail: String) = DiagnosticItem(label, detail, DiagnosticSeverity.OK)
    private fun warn(label: String, detail: String) = DiagnosticItem(label, detail, DiagnosticSeverity.WARN)
    private fun fail(label: String, detail: String) = DiagnosticItem(label, detail, DiagnosticSeverity.FAIL)

    private companion object {
        val MODULE_DIR = "/data/adb/modules/${AvcrtPaths.MODULE_ID}"

        /** Single-instance lock the daemon flock()s — see avcrtd.cpp. */
        const val DAEMON_LOCK = "/data/local/tmp/avcrtd.lock"
    }
}
