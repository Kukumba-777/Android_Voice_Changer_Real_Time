package dev.avcrt.engine.hal

import dev.avcrt.core.common.AvcrtPaths
import dev.avcrt.core.common.dispatcher.DispatcherProvider
import dev.avcrt.core.common.result.AvcrtError
import dev.avcrt.core.common.result.AvcrtResult
import dev.avcrt.core.common.result.map
import dev.avcrt.core.root.RootAccess
import dev.avcrt.core.root.RootStatus
import dev.avcrt.domain.engine.EngineState
import dev.avcrt.domain.engine.VoiceEnginePort
import dev.avcrt.domain.preset.Preset
import dev.avcrt.domain.preset.PresetRepository
import dev.avcrt.domain.settings.SettingsRepository
import dev.avcrt.engine.api.EngineConfigDto
import dev.avcrt.engine.api.EngineDspDto
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.launchIn
import kotlinx.coroutines.flow.onEach
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import kotlinx.serialization.json.Json
import timber.log.Timber
import javax.inject.Inject
import javax.inject.Singleton

/**
 * The high-level engine controller.
 *
 * The actual DSP runs inside the KSU module's `libavcrtfx.so` audio effect.
 * Our job here is to:
 *   - tell the module which preset is active by writing JSON config
 *   - flip the engine on/off via a sentinel file
 *   - bump a system property the native side polls so it reloads immediately
 *   - install / uninstall the bundled module zip when the user requests it
 */
@Singleton
class HalEngine @Inject constructor(
    private val rootAccess: RootAccess,
    private val moduleInstaller: ModuleInstaller,
    private val presets: PresetRepository,
    private val settings: SettingsRepository,
    private val dispatchers: DispatcherProvider,
) : VoiceEnginePort {

    private val json = Json {
        ignoreUnknownKeys = true
        encodeDefaults = true
        prettyPrint = false
    }

    private val state = MutableStateFlow<EngineState>(EngineState.Idle)
    private val mutex = Mutex()
    private var revision = 0L

    // True while a live preview (editor "Apply & test") is overriding the runtime config; stopPreview
    // restores the persisted active preset. Any real apply / master-switch toggle clears it.
    @Volatile private var previewing = false

    private val scope = CoroutineScope(SupervisorJob() + dispatchers.io)

    init {
        // Keep state aligned with persisted preset whenever it changes.
        combine(
            settings.observe(),
            presets.observeAll(),
            rootAccess.status,
        ) { s, all, root -> Triple(s, all, root) }
            .onEach { (s, all, _) -> reconcileState(s.engineEnabled, s.activePresetId, all) }
            .launchIn(scope)

        // One-shot runtime sync on the first root-grant: re-write the live config so the cellular
        // daemon + the Zygisk hook pick up the persisted "engine on" + active preset WITHOUT a
        // manual toggle. On a fresh install/reboot, customize.sh resets active_preset.json to
        // disabled (safe default), so the runtime was stale vs. the user's persisted intent until
        // they toggled the master switch off→on (which calls writeConfig). This reconciles it
        // automatically. (engineEnabled defaults to false, so a genuinely-off user stays off.)
        scope.launch {
            rootAccess.status.first { it is RootStatus.Available && it.granted }
            runCatching {
                if (settings.snapshot().engineEnabled) {
                    Timber.i("startup sync: re-applying persisted engine-on config")
                    mutate(enable = true)
                }
            }.onFailure { Timber.w(it, "startup config sync failed") }
        }
    }

    override fun observeState(): StateFlow<EngineState> = state.asStateFlow()

    override suspend fun refreshState(): AvcrtResult<EngineState> = withContext(dispatchers.io) {
        // requiresReboot is now driven by the install-pending marker file that
        // the module's post-fs-data.sh deletes on every boot — so it's already
        // authoritative. No “module dir exists, therefore we rebooted” heuristic
        // needed (KSU installs the dir BEFORE reboot, so the heuristic was wrong).
        val installed = moduleInstaller.isInstalled()
        val rebootNeeded = moduleInstaller.requiresReboot()
        val s = settings.snapshot()
        val all = presets.observeAll().first()
        val newState = computeState(
            engineEnabled = s.engineEnabled,
            activePresetId = s.activePresetId,
            allPresets = all,
            moduleInstalled = installed,
            requiresReboot = rebootNeeded,
        )
        state.value = newState
        AvcrtResult.Success(newState)
    }

    override suspend fun start(): AvcrtResult<Unit> = withRoot { mutate(enable = true) }

    override suspend fun stop(): AvcrtResult<Unit> = withRoot { mutate(enable = false) }

    override suspend fun applyPreset(preset: Preset): AvcrtResult<Unit> = withRoot {
        previewing = false
        settings.setActivePresetId(preset.id)
        writeConfig(
            enabled = state.value is EngineState.Running,
            preset = preset,
        )
    }

    override suspend fun previewPreset(preset: Preset): AvcrtResult<Unit> = withRoot {
        // enabled = true so it's audible live even if the master switch is off; deliberately does NOT
        // touch settings (activePresetId / engineEnabled) — stopPreview puts the real config back.
        previewing = true
        writeConfig(enabled = true, preset = preset)
    }

    override fun stopPreview() {
        if (!previewing) return
        previewing = false
        // The editor VM scope is gone by the time this runs (screen closed), so restore on the
        // engine's own @Singleton scope.
        scope.launch {
            runCatching {
                val s = settings.snapshot()
                val all = presets.observeAll().first()
                val active = s.activePresetId?.let { id -> all.firstOrNull { it.id == id } }
                    ?: all.firstOrNull()
                if (active != null) writeConfig(enabled = s.engineEnabled, preset = active)
            }.onFailure { Timber.w(it, "stopPreview restore failed") }
        }
    }

    override suspend fun installModule(): AvcrtResult<Unit> = withRoot {
        moduleInstaller.installFromAssets().also { result ->
            if (result is AvcrtResult.Success) {
                state.value = EngineState.RebootRequired
            }
        }
    }

    override suspend fun uninstallModule(): AvcrtResult<Unit> = withRoot {
        moduleInstaller.uninstall().map { Unit }
    }

    override suspend fun reboot(): AvcrtResult<Unit> = withRoot {
        rootAccess.exec("svc power reboot")
        AvcrtResult.Success(Unit)
    }

    private suspend fun mutate(enable: Boolean): AvcrtResult<Unit> = mutex.withLock {
        previewing = false
        val s = settings.snapshot()
        val all = presets.observeAll().first()
        val preset = s.activePresetId?.let { id -> all.firstOrNull { it.id == id } }
            ?: all.firstOrNull()
            ?: return AvcrtResult.Failure(AvcrtError.Unknown("No preset available"))
        val res = writeConfig(enabled = enable, preset = preset)
        if (res is AvcrtResult.Success) settings.setEngineEnabled(enable)
        res
    }

    private suspend fun writeConfig(enabled: Boolean, preset: Preset): AvcrtResult<Unit> {
        revision++
        val dto = EngineConfigDto(
            enabled = enabled,
            presetId = preset.id,
            dsp = EngineDspDto.from(preset.dsp),
            revision = revision,
        )
        val payload = json.encodeToString(EngineConfigDto.serializer(), dto)
        val write = rootAccess.writeFile(AvcrtPaths.ACTIVE_PRESET_FILE, payload, "0644")
        if (write is AvcrtResult.Failure) return write
        // Sentinel file so the native module can fail-fast without parsing JSON.
        rootAccess.writeFile(AvcrtPaths.ENGINE_STATE_FILE, if (enabled) "on" else "off", "0644")
        // Mirror the DSP config to system properties so the Zygisk app-capture
        // hook (which can't read the preset JSON from an app sandbox) sees it, then
        // bump the revision — both the cellular daemon and the hook re-read on this.
        val d = preset.dsp
        val fx = d.fx
        fun b(x: Boolean) = if (x) 1 else 0
        rootAccess.exec(
            buildString {
                append("setprop ${AvcrtPaths.PROP_ENABLED} ${if (enabled) 1 else 0}; ")
                append("setprop ${AvcrtPaths.PROP_PITCH} ${d.pitchSemitones}; ")
                append("setprop ${AvcrtPaths.PROP_FORMANT} ${d.formantSemitones}; ")
                append("setprop ${AvcrtPaths.PROP_GAIN} ${d.gainDb}; ")
                append("setprop ${AvcrtPaths.PROP_PRESERVE_FORMANTS} ${if (d.preserveFormants) 1 else 0}; ")
                append("setprop ${AvcrtPaths.PROP_CRISPNESS} ${d.crispness}; ")
                // Phase-6 effect props (grouped CSV; field order = the native parse_fx_props contract).
                append(
                    "setprop ${AvcrtPaths.PROP_FX_GATE} " +
                        "\"${b(fx.gate.enabled)},${fx.gate.thresholdDb},${fx.gate.attackMs},${fx.gate.releaseMs}\"; ",
                )
                append(
                    "setprop ${AvcrtPaths.PROP_FX_EQ} " +
                        "\"${b(fx.eq.enabled)},${fx.eq.lowDb},${fx.eq.midDb},${fx.eq.highDb},${b(fx.telephone.enabled)}\"; ",
                )
                append(
                    "setprop ${AvcrtPaths.PROP_FX_MOD} " +
                        "\"${b(fx.ringmod.enabled)},${fx.ringmod.carrierHz},${fx.ringmod.mix}," +
                        "${b(fx.tremolo.enabled)},${fx.tremolo.rateHz},${fx.tremolo.depth},${fx.tremolo.shape}\"; ",
                )
                append(
                    "setprop ${AvcrtPaths.PROP_FX_DRIVE} " +
                        "\"${b(fx.distortion.enabled)},${fx.distortion.drive},${fx.distortion.mix}\"; ",
                )
                append(
                    "setprop ${AvcrtPaths.PROP_FX_TIME} " +
                        "\"${b(fx.echo.enabled)},${fx.echo.timeMs},${fx.echo.feedback},${fx.echo.mix}\"; ",
                )
                append(
                    "setprop ${AvcrtPaths.PROP_FX_VERB} " +
                        "\"${b(fx.reverb.enabled)},${fx.reverb.roomSize},${fx.reverb.damping}," +
                        "${fx.reverb.wet},${fx.reverb.width}\"; ",
                )
                // ---- Tier-2 grouped props (field order = the native parse_fx_props contract) ----
                append(
                    "setprop ${AvcrtPaths.PROP_FX_FILTER} " +
                        "\"${b(fx.filter.enabled)},${fx.filter.hpHz},${fx.filter.lpHz},${fx.filter.q}\"; ",
                )
                append(
                    "setprop ${AvcrtPaths.PROP_FX_MOD2} " +
                        "\"${b(fx.vibrato.enabled)},${fx.vibrato.rateHz},${fx.vibrato.depth}," +
                        "${b(fx.chorus.enabled)},${fx.chorus.rateHz},${fx.chorus.depth},${fx.chorus.mix}\"; ",
                )
                append(
                    "setprop ${AvcrtPaths.PROP_FX_FLANGER} " +
                        "\"${b(fx.flanger.enabled)},${fx.flanger.rateHz},${fx.flanger.depth}," +
                        "${fx.flanger.feedback},${fx.flanger.mix}\"; ",
                )
                append(
                    "setprop ${AvcrtPaths.PROP_FX_PHASER} " +
                        "\"${b(fx.phaser.enabled)},${fx.phaser.rateHz},${fx.phaser.depth}," +
                        "${fx.phaser.feedback},${fx.phaser.mix}\"; ",
                )
                append(
                    "setprop ${AvcrtPaths.PROP_FX_WAH} " +
                        "\"${b(fx.autowah.enabled)},${fx.autowah.sensitivity},${fx.autowah.baseHz}," +
                        "${fx.autowah.rangeHz},${fx.autowah.mix}\"; ",
                )
                append(
                    "setprop ${AvcrtPaths.PROP_FX_CRUSH} " +
                        "\"${b(fx.bitcrush.enabled)},${fx.bitcrush.bits},${fx.bitcrush.downsample}," +
                        "${fx.bitcrush.mix}\"; ",
                )
                append(
                    "setprop ${AvcrtPaths.PROP_FX_COMP} " +
                        "\"${b(fx.compressor.enabled)},${fx.compressor.thresholdDb},${fx.compressor.ratio}," +
                        "${fx.compressor.attackMs},${fx.compressor.releaseMs},${fx.compressor.makeupDb}\"; ",
                )
                // ---- Tier-3 grouped props (field order = the native parse_fx_props contract) ----
                append(
                    "setprop ${AvcrtPaths.PROP_FX_FORMANT} " +
                        "\"${b(fx.formant.enabled)},${fx.formant.shift}\"; ",
                )
                append(
                    "setprop ${AvcrtPaths.PROP_FX_VOCODER} " +
                        "\"${b(fx.vocoder.enabled)},${fx.vocoder.carrierHz},${fx.vocoder.carrierType},${fx.vocoder.mix}\"; ",
                )
                append(
                    "setprop ${AvcrtPaths.PROP_FX_WHISPER} " +
                        "\"${b(fx.whisper.enabled)},${fx.whisper.mix}\"; ",
                )
                // ---- Phase 7 experimental spectral (field order = native parse_fx_props contract) ----
                append(
                    "setprop ${AvcrtPaths.PROP_FX_SPECTRAL} " +
                        "\"${b(fx.spectral.robot)},${b(fx.spectral.whisper)},${b(fx.spectral.freeze)}," +
                        "${b(fx.spectral.blur)},${fx.spectral.blurAmt},${fx.spectral.mix}\"; ",
                )
                append(
                    "setprop ${AvcrtPaths.PROP_FX_AUTOTUNE} " +
                        "\"${b(fx.autotune.enabled)},${fx.autotune.strength}\"; ",
                )
                append(
                    "setprop ${AvcrtPaths.PROP_FX_HARMON} " +
                        "\"${b(fx.harmonizer.enabled)},${fx.harmonizer.interval1},${fx.harmonizer.interval2}," +
                        "${fx.harmonizer.mix}\"; ",
                )
                // revision LAST so the native side sees a complete config when it reloads.
                append("setprop ${AvcrtPaths.RELOAD_PROPERTY} ${dto.revision}")
            },
        )
        return AvcrtResult.Success(Unit)
    }

    private suspend fun reconcileState(
        enabled: Boolean,
        activePresetId: String?,
        allPresets: List<Preset>,
    ) {
        val installed = moduleInstaller.isInstalled()
        val rebootNeeded = moduleInstaller.requiresReboot()
        state.value = computeState(enabled, activePresetId, allPresets, installed, rebootNeeded)
    }

    private fun computeState(
        engineEnabled: Boolean,
        activePresetId: String?,
        allPresets: List<Preset>,
        moduleInstalled: Boolean,
        requiresReboot: Boolean,
    ): EngineState {
        val active = activePresetId?.let { id -> allPresets.firstOrNull { it.id == id } }
        return when {
            !moduleInstalled -> EngineState.ModuleNotInstalled
            requiresReboot -> EngineState.RebootRequired
            engineEnabled && active != null -> EngineState.Running(active)
            else -> EngineState.ReadyDisabled(active)
        }
    }

    private suspend inline fun withRoot(block: () -> AvcrtResult<Unit>): AvcrtResult<Unit> {
        if (rootAccess.currentStatus() !is RootStatus.Available) {
            val request = rootAccess.requestAccess()
            if (request is AvcrtResult.Failure) return request
        }
        return try {
            block()
        } catch (t: Throwable) {
            Timber.e(t, "HalEngine command failed")
            AvcrtResult.Failure(AvcrtError.Unknown(t.message ?: "Engine command failed", t))
        }
    }
}
