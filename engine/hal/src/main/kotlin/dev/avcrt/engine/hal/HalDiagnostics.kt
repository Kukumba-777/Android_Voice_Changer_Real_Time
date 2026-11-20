package dev.avcrt.engine.hal

import dev.avcrt.core.common.AvcrtPaths
import dev.avcrt.core.common.dispatcher.DispatcherProvider
import dev.avcrt.core.common.result.AvcrtError
import dev.avcrt.core.common.result.AvcrtResult
import dev.avcrt.core.root.RootAccess
import dev.avcrt.core.root.RootProvider
import dev.avcrt.core.root.RootStatus
import dev.avcrt.engine.api.DiagnosticItem
import dev.avcrt.engine.api.DiagnosticReport
import dev.avcrt.engine.api.DiagnosticSeverity
import dev.avcrt.engine.api.EngineDiagnostics
import kotlinx.coroutines.withContext
import org.json.JSONObject
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
    private val moduleRuntime: ModuleRuntime,
) : EngineDiagnostics {

    override suspend fun run(): AvcrtResult<DiagnosticReport> = withContext(dispatchers.io) {
        val status = rootAccess.currentStatus()
        if (status !is RootStatus.Available || !status.granted) {
            return@withContext AvcrtResult.Failure(AvcrtError.RootDenied("Diagnostics need root"))
        }

        val module = moduleRuntime.inspect()
        val items = buildList {
            add(checkRootProvider())
            add(checkModuleRuntime(module, status.provider))
            add(checkKernelPatch())
            add(checkCvpRebuildControl())
            add(checkDaemonRunning())
            add(checkDaemonLogTail())
            add(checkZygiskHook())
            add(checkCaptureRuntime())
            add(checkAppDspConfig())
            add(checkModuleDirectory())
            add(checkModuleContents())
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
     * Checks the codec mute capability, not end-to-end cellular compatibility.
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
                "AVCRT Dec Mute kcontrol present — codec mute capability available; " +
                    "cellular routing still requires device-specific testing." +
                    if (ourKernel) " (/proc/version is an avcrt build)" else "",
            )
        } else {
            warn(
                "Kernel patch",
                "AVCRT Dec Mute not detected (or probe failed). Cellular clean-replace requires " +
                    "a matching device kernel patch and route validation. App-level Zygisk processing " +
                    "is separate and does not require this control.",
            )
        }
    }

    private suspend fun checkCvpRebuildControl(): DiagnosticItem {
        val bin = "$MODULE_DIR/bin/avcrtd"
        if (!rootAccess.fileExists(bin)) {
            return warn("CVP rebuild", "avcrtd binary missing — install the module first.")
        }
        // Older daemons ignore unknown arguments and START instead of probing.
        // Check a new CLI marker without executing the binary before using a new flag.
        val supported = sh("grep -a -q -- '--list-probes' '$bin' && echo supported")
        if (supported != "supported") {
            return warn("CVP rebuild", "Installed avcrtd has no safe probe capability marker. " +
                "Update the module; CVP control availability is unknown (daemon was not executed).")
        }
        val result = sh("'$bin' --probe-cvp >/dev/null 2>&1; echo \$?")
        return when (result) {
            "0" -> ok("CVP rebuild", "AVCRT CVP Rebuild control present. This does not prove " +
                "successful recovery for every accessory transition.")
            "1" -> warn("CVP rebuild", "AVCRT CVP Rebuild control absent on the inspected card. " +
                "This does not establish cellular support. Inspect daemon logs for route recovery.")
            else -> warn("CVP rebuild", "Unable to inspect CVP control (probe result: $result). " +
                "This is not evidence that the control is absent.")
        }
    }

    private suspend fun checkDaemonRunning(): DiagnosticItem {
        val pid = sh("pidof avcrtd 2>/dev/null")
        return when {
            pid.isNotBlank() -> ok("Daemon", "avcrtd running (pid $pid)")
            else -> warn(
                "Daemon",
                "No running avcrtd process found. A lock file alone does not show a running daemon.",
            )
        }
    }

    private suspend fun checkDaemonLogTail(): DiagnosticItem {
        val tail = sh("${AvcrtPaths.CURRENT_DAEMON_LOG_GUARD} && tail -n 8 '${AvcrtPaths.DAEMON_LOG_FILE}'")
        return if (tail.isBlank()) {
            warn("Daemon log", "No daemon log verified for this boot.")
        } else {
            ok("Daemon log (last 8)", tail)
        }
    }

    private suspend fun checkCaptureRuntime(): DiagnosticItem {
        // Keep a separate tail for each kind of evidence: unrelated app starts
        // must not crowd the last real recording out of the diagnostic summary.
        val command = """
            (set -o pipefail; logcat -d -v threadtime -s avcrtzy:I avcrtaf:I | awk '
                /java-read FIRED |AF PCM processed / { kind = "pcm" }
                !/java-read FIRED |AF PCM processed / && / FIRED / { kind = "read" }
                /avcrt zygisk init/ { kind = "init" }
                /AudioRecord JNI hooks:/ { kind = "jni" }
                kind != "" { rows[kind, ++count[kind] % 5] = ${'$'}0; kind = "" }
                END {
                    for (key in count)
                        for (i = (count[key] > 5 ? count[key] - 4 : 1); i <= count[key]; i++)
                            print rows[key, i % 5]
                }
            ')
        """.trimIndent()
        val log = rootAccess.exec(command) as? AvcrtResult.Success
            ?: return warn("App capture runtime", "Unable to read capture logs.")
        val lines = log.value.lineSequence().toList()
        // Legacy native FIRED messages can precede format validation and DSP.
        val processed = lines.filter { "java-read FIRED " in it || "AF PCM processed " in it }.takeLast(5)
        val init = lines.filter { "avcrt zygisk init" in it }.takeLast(5)
        val hookSummary = lines.filter { "AudioRecord JNI hooks:" in it }.takeLast(5)
        val readEvents = lines.filter { " FIRED " in it }.takeLast(5)
        return when {
            processed.isNotEmpty() -> ok("PCM processing logged (historical; see PID and time)", processed.joinToString("\n"))
            readEvents.isNotEmpty() -> warn("App capture runtime", "Read hook observed; DSP not confirmed.\n" + readEvents.joinToString("\n"))
            init.isNotEmpty() || hookSummary.isNotEmpty() -> warn("App capture runtime", "Zygisk loaded AVCRT in at least one process, but no processed frames were logged. Compare the PID with the app doing the recording; check the JNI hook counts and DSP configuration.\n" + (init + hookSummary).joinToString("\n"))
            else -> warn("App capture runtime", "No AVCRT capture events in logcat. If a fresh recording with the engine on still produces no events, check the Zygisk loader and target-app exclusions. An empty buffer alone does not prove the loader is disabled.")
        }
    }

    /** Checks packaging only; loader activity is not inferred from the files. */
    private suspend fun checkZygiskHook(): DiagnosticItem {
        val supported = android.os.Build.SUPPORTED_ABIS.toList()
        val installed = supported.filter { rootAccess.fileExists("$MODULE_DIR/zygisk/$it.so") }
        return if (installed.isEmpty()) warn("Zygisk payload", "No hook library for the device's reported ABIs.")
        else ok("Zygisk payload", "Installed for ${installed.joinToString()}; see capture runtime for evidence of loading.")
    }

    private suspend fun checkModuleDirectory(): DiagnosticItem {
        val activeProp = "$MODULE_DIR/module.prop"
        val pendingProp = "${AvcrtPaths.STAGED_MODULE_DIR}/module.prop"
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
        val dir = if (rootAccess.fileExists("$MODULE_DIR/module.prop")) MODULE_DIR else AvcrtPaths.STAGED_MODULE_DIR
        val listing = sh("find '$dir' -maxdepth 3 -type f 2>/dev/null | sort")
        val hasDaemon = listing.lineSequence().any { it == "$dir/bin/avcrtd" }
        val hasZygisk = android.os.Build.SUPPORTED_ABIS.any { abi ->
            listing.lineSequence().any { it == "$dir/zygisk/$abi.so" }
        }
        val truncated = listing.lineSequence().take(30).joinToString("\n")
        return when {
            !hasDaemon && !hasZygisk -> fail(
                "Module contents",
                "Neither bin/avcrtd nor a compatible Zygisk library is inside the module — the bundled zip was " +
                    "empty or staging failed. Rebuild the APK from a clean state.\n--- listing ---\n$truncated",
            )
            !hasDaemon -> warn(
                "Module contents",
                "zygisk hook present but bin/avcrtd MISSING — the cellular daemon can't run." +
                    "\n--- listing ---\n$truncated",
            )
            !hasZygisk -> warn(
                "Module contents",
                "avcrtd present but no compatible Zygisk library." +
                    "\n--- listing ---\n$truncated",
            )
            else -> ok(
                "Module contents",
                "Daemon and compatible Zygisk payload present.\n--- listing ---\n$truncated",
            )
        }
    }

    private fun checkModuleRuntime(state: ModuleRuntime.State, provider: RootProvider): DiagnosticItem = when (state) {
        ModuleRuntime.State.REBOOT -> warn("Module activation", "Module change pending reboot.")
        ModuleRuntime.State.DISABLED -> warn("Module activation", "Disabled in the root manager.")
        ModuleRuntime.State.NOT_STARTED -> warn("Module activation", "Files present, service not observed this boot. Check the root manager boot log.")
        ModuleRuntime.State.NOT_STARTED_WITH_ZYGISK -> {
            val hint = when (provider) {
                RootProvider.MAGISK -> "Some Magisk builds skip modules containing zygisk/ when their loader is inactive. Check the loader settings and the manager boot log."
                RootProvider.KERNELSU -> "Check the KernelSU boot log for the missing service. App/VoIP capture separately needs a working Zygisk loader such as Zygisk Next."
                else -> "Check the manager boot log and whether its Zygisk loader is active."
            }
            warn("Module activation", "Module files present, but the service was not observed this boot. $hint")
        }
        ModuleRuntime.State.EARLY_BOOT_ONLY -> warn("Module activation", "The early boot script ran, but the late-start service was not observed. Check the manager boot log.")
        ModuleRuntime.State.SERVICE_SEEN -> warn("Module activation", "Service ran this boot but no daemon process was found.")
        ModuleRuntime.State.STARTED -> ok("Module activation", "Daemon process found; capture is checked separately.")
        ModuleRuntime.State.MISSING -> warn("Module activation", "Module files not found.")
        ModuleRuntime.State.UNKNOWN -> warn("Module activation", "Unable to verify module state.")
    }

    private suspend fun checkAppDspConfig(): DiagnosticItem {
        val saved = rootAccess.readFile(AvcrtPaths.ACTIVE_PRESET_FILE) as? AvcrtResult.Success
            ?: return warn("App DSP config", "Cannot read the saved module configuration.")
        val config = runCatching { JSONObject(saved.value) }.getOrNull()
            ?: return warn("App DSP config", "Saved module configuration is not valid JSON.")
        val enabled = config.opt("enabled") as? Boolean
            ?: return warn("App DSP config", "Saved module configuration has no valid enabled flag.")
        val revision = config.optLong("revision", -1)
        val live = (rootAccess.exec("getprop ${AvcrtPaths.PROP_ENABLED}; getprop ${AvcrtPaths.RELOAD_PROPERTY}")
            as? AvcrtResult.Success)?.value?.lines()
            ?: return warn("App DSP config", "Could not read live DSP properties.")
        val expectedEnabled = if (enabled) "1" else "0"
        if (live.getOrNull(0) != expectedEnabled || revision < 0 || live.getOrNull(1)?.toLongOrNull() != revision) {
            return warn("App DSP config", "Saved and live DSP settings do not match. Boot-time restore or a recent config write may be incomplete; this does not indicate whether Zygisk loaded.")
        }
        return ok("App DSP config", "Saved and live enabled/revision markers match (enabled=$enabled, revision=$revision). Capture is checked separately.")
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
        const val MODULE_DIR = AvcrtPaths.MODULE_DIR
    }
}
