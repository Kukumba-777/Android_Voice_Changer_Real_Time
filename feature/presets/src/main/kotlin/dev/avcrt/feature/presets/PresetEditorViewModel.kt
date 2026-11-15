package dev.avcrt.feature.presets

import androidx.lifecycle.SavedStateHandle
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import dagger.hilt.android.lifecycle.HiltViewModel
import dev.avcrt.core.common.result.AvcrtResult
import dev.avcrt.domain.engine.VoiceEnginePort
import dev.avcrt.domain.preset.Preset
import dev.avcrt.domain.preset.PresetDsp
import dev.avcrt.domain.preset.PresetEffects
import dev.avcrt.domain.preset.PresetOrigin
import dev.avcrt.domain.preset.PresetRepository
import dev.avcrt.domain.settings.SettingsRepository
import dev.avcrt.domain.usecase.ApplyPresetUseCase
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import java.util.Locale
import java.util.UUID
import javax.inject.Inject

data class PresetEditorUiState(
    val isEditing: Boolean = false,
    val loading: Boolean = true,
    val name: String = "",
    val description: String = "",
    val pitch: Float = 0f,
    val formant: Float = 0f,
    val gainDb: Float = 0f,
    val crispness: Int = 5,
    val preserveFormants: Boolean = true,
    val effects: PresetEffects = PresetEffects(),
    /** When true (Settings → Experimental), the editor reveals the experimental effect cards. */
    val experimentalUnlocked: Boolean = false,
    val applyAfterSave: Boolean = true,
    val nameError: Boolean = false,
    val message: String? = null,
    /** In-app "Test" (record → listen) lifecycle. */
    val previewPhase: PreviewPhase = PreviewPhase.Idle,
    /** Seconds elapsed while [PreviewPhase.Recording] (for the countdown UI). */
    val recordElapsed: Int = 0,
    /** One-shot: set when the editor is done (saved/deleted) → screen pops back. */
    val finished: Boolean = false,
) {
    val canPlayPreview: Boolean get() = previewPhase == PreviewPhase.Recorded
}

/** Editor "Test" button states: idle → recording (mic, effect applied in-process) → recorded → playing. */
enum class PreviewPhase { Idle, Recording, Recorded, Playing }

/** Test-clip length (seconds). Shared by the VM (record) and the screen (countdown UI). */
internal const val PREVIEW_RECORD_SECONDS = 6

/**
 * Backs [PresetEditorScreen]. Two modes, chosen by the optional `presetId` nav arg:
 *   - null  → create a new custom preset
 *   - non-null → edit the existing custom preset with that id
 *
 * The editor only exposes the DSP params the native engine actually applies
 * (pitch, formant, gain, preserve-formants — see avcrt_engine.cpp). timeRatio and
 * crispness are intentionally NOT surfaced: timeRatio breaks the realtime feed and
 * neither is plumbed to the runtime; their values are carried through untouched
 * via [baseDsp] so editing never silently drops them.
 *
 * Name/description are localized maps. We edit only the CURRENT-locale entry and
 * preserve the others ([baseNames]/[baseDescriptions]) — so duplicating a built-in
 * (which ships en+ru) keeps the device-locale text and saving never collapses the
 * map to a single wrong-language string.
 */
@HiltViewModel
class PresetEditorViewModel @Inject constructor(
    private val presets: PresetRepository,
    private val applyPreset: ApplyPresetUseCase,
    private val engine: VoiceEnginePort,
    private val recorder: PreviewRecorder,
    settings: SettingsRepository,
    savedStateHandle: SavedStateHandle,
) : ViewModel() {

    private val presetId: String? = savedStateHandle[PresetEditorRoute.ARG_PRESET_ID]

    // The app's active language (AppCompat per-app locale → Locale.getDefault on
    // API 33+), matching what the preset list uses to display names.
    private val localeTag: String = Locale.getDefault().language.ifBlank { "en" }

    // Stable id used for the live preview ("Apply & test") — the real id when editing, a throwaway
    // when creating (never persisted; stopPreview restores the real active preset on exit).
    private val previewId: String = presetId ?: "custom.preview"

    // In-app test (record → listen). The captured PCM lives here (off the UiState — it's large).
    private var previewJob: Job? = null
    private var recorded: ShortArray? = null

    // Fields the editor doesn't expose, preserved across an edit.
    private var baseDsp: PresetDsp = PresetDsp(pitchSemitones = 0.0, formantSemitones = 0.0)
    private var baseIconKey: String = DEFAULT_ICON_KEY
    private var baseTags: List<String> = listOf(CUSTOM_TAG)
    private var baseNames: Map<String, String> = emptyMap()
    private var baseDescriptions: Map<String, String> = emptyMap()

    private val _state = MutableStateFlow(
        PresetEditorUiState(
            isEditing = presetId != null,
            experimentalUnlocked = settings.snapshot().experimentalEnabled,
        ),
    )
    val state: StateFlow<PresetEditorUiState> = _state.asStateFlow()

    init {
        if (presetId == null) {
            _state.update { it.copy(loading = false) }
        } else {
            viewModelScope.launch {
                val existing = presets.observeAll().first().firstOrNull { it.id == presetId }
                if (existing == null) {
                    // Vanished (deleted elsewhere) — nothing to edit.
                    _state.update { it.copy(loading = false, finished = true) }
                    return@launch
                }
                baseDsp = existing.dsp
                baseIconKey = existing.iconKey
                baseTags = existing.tags
                baseNames = existing.names
                baseDescriptions = existing.descriptions
                _state.update {
                    it.copy(
                        loading = false,
                        name = existing.displayName(localeTag),
                        description = existing.displayDescription(localeTag) ?: "",
                        pitch = existing.dsp.pitchSemitones.toFloat(),
                        formant = existing.dsp.formantSemitones.toFloat(),
                        gainDb = existing.dsp.gainDb.toFloat(),
                        crispness = existing.dsp.crispness,
                        preserveFormants = existing.dsp.preserveFormants,
                        effects = existing.dsp.fx,
                    )
                }
            }
        }
    }

    fun onName(value: String) = _state.update { it.copy(name = value, nameError = false) }
    fun onDescription(value: String) = _state.update { it.copy(description = value) }
    fun onPitch(value: Float) = _state.update { it.copy(pitch = value) }
    fun onFormant(value: Float) = _state.update { it.copy(formant = value) }
    fun onGain(value: Float) = _state.update { it.copy(gainDb = value) }
    fun onCrispness(value: Int) = _state.update { it.copy(crispness = value) }
    fun onPreserveFormants(value: Boolean) = _state.update { it.copy(preserveFormants = value) }
    fun onApplyAfterSave(value: Boolean) = _state.update { it.copy(applyAfterSave = value) }

    /** Immutable update of the effect chain, e.g. updateEffects { it.copy(reverb = it.reverb.copy(...)) }. */
    fun updateEffects(transform: (PresetEffects) -> PresetEffects) =
        _state.update { it.copy(effects = transform(it.effects)) }

    fun onReset() = _state.update {
        it.copy(pitch = 0f, formant = 0f, gainDb = 0f, crispness = 5, preserveFormants = true, effects = PresetEffects())
    }

    fun onSave() {
        val s = _state.value
        val name = s.name.trim()
        if (name.isEmpty()) {
            _state.update { it.copy(nameError = true) }
            return
        }
        val description = s.description.trim()
        val preset = Preset(
            id = presetId ?: "custom.${UUID.randomUUID()}",
            origin = PresetOrigin.CUSTOM,
            names = baseNames + (localeTag to name),
            descriptions = if (description.isEmpty()) {
                baseDescriptions - localeTag
            } else {
                baseDescriptions + (localeTag to description)
            },
            iconKey = baseIconKey,
            dsp = baseDsp.copy(
                pitchSemitones = s.pitch.toDouble(),
                formantSemitones = s.formant.toDouble(),
                gainDb = s.gainDb.toDouble(),
                crispness = s.crispness,
                preserveFormants = s.preserveFormants,
                fx = s.effects,
            ),
            tags = baseTags.ifEmpty { listOf(CUSTOM_TAG) },
        )
        viewModelScope.launch {
            when (val res = presets.upsertCustom(preset)) {
                is AvcrtResult.Success -> {
                    if (s.applyAfterSave) applyPreset(preset.id)
                    _state.update { it.copy(finished = true) }
                }
                is AvcrtResult.Failure ->
                    _state.update { it.copy(message = res.error.message) }
            }
        }
    }

    fun onDelete() {
        val id = presetId ?: return
        viewModelScope.launch {
            when (val res = presets.delete(id)) {
                is AvcrtResult.Success -> _state.update { it.copy(finished = true) }
                is AvcrtResult.Failure ->
                    _state.update { it.copy(message = res.error.message) }
            }
        }
    }

    /**
     * "Test" button: apply the current UNSAVED preset to the runtime, record a few seconds of the
     * user's own mic (the system-wide Zygisk hook voice-changes it in THIS process), restore the real
     * config, then expose the captured clip for playback. Lets the user audition a preset without it
     * going out on calls/other apps.
     */
    fun onStartTest() {
        val phase = _state.value.previewPhase
        if (phase == PreviewPhase.Recording || phase == PreviewPhase.Playing) return
        previewJob?.cancel()
        previewJob = viewModelScope.launch {
            when (val applied = engine.previewPreset(buildPreviewPreset())) {
                is AvcrtResult.Failure -> {
                    _state.update { it.copy(message = applied.error.message, previewPhase = PreviewPhase.Idle) }
                    return@launch
                }
                is AvcrtResult.Success -> Unit
            }
            _state.update { it.copy(previewPhase = PreviewPhase.Recording, recordElapsed = 0) }
            // Give the hook a moment to pick up the new revision before we capture.
            delay(PROP_SETTLE_MS)
            val ticker = launch {
                for (s in 1..PREVIEW_RECORD_SECONDS) {
                    delay(1000)
                    _state.update { it.copy(recordElapsed = s) }
                }
            }
            // On cancellation (user hit Stop / left): rethrow so this coroutine ends WITHOUT touching
            // state — onStopTest owns the early-stop transition (discards the clip). Only a full
            // capture reaches the Recorded state below.
            val buf = try {
                recorder.record(PREVIEW_RECORD_SECONDS)
            } catch (e: CancellationException) {
                throw e
            } catch (e: Throwable) {
                null
            }
            ticker.cancel()
            engine.stopPreview()
            recorded = buf?.takeIf { it.isNotEmpty() }
            _state.update {
                it.copy(previewPhase = if (recorded != null) PreviewPhase.Recorded else PreviewPhase.Idle)
            }
        }
    }

    /** Plays back the captured test clip (already voice-changed). */
    fun onPlayTest() {
        val buf = recorded ?: return
        if (_state.value.previewPhase == PreviewPhase.Playing) return
        previewJob?.cancel()
        previewJob = viewModelScope.launch {
            _state.update { it.copy(previewPhase = PreviewPhase.Playing) }
            runCatching { recorder.play(buf) }
            _state.update { it.copy(previewPhase = PreviewPhase.Recorded) }
        }
    }

    /**
     * "Stop" button. While Recording: finish early but KEEP what was captured — the record coroutine
     * returns the partial clip and moves to Recorded, so the user can listen to those few seconds.
     * While Playing: just stop playback (the clip stays available to replay).
     */
    fun onStopTest() {
        when (_state.value.previewPhase) {
            PreviewPhase.Recording -> recorder.requestStop()
            PreviewPhase.Playing -> {
                previewJob?.cancel()
                previewJob = null
                recorder.stop()
                _state.update { it.copy(previewPhase = PreviewPhase.Recorded) }
            }
            else -> Unit
        }
    }

    /** Editor closed — hard-stop any audio and restore the real active preset (discards the test clip). */
    fun onLeaveEditor() {
        previewJob?.cancel()
        previewJob = null
        recorder.stop()
        engine.stopPreview()
    }

    override fun onCleared() {
        // Editor closed — stop any audio and restore the real active preset's config.
        recorder.stop()
        engine.stopPreview()
    }

    private fun buildPreviewPreset(): Preset {
        val s = _state.value
        return Preset(
            id = previewId,
            origin = PresetOrigin.CUSTOM,
            names = baseNames + (localeTag to s.name.trim().ifBlank { "preview" }),
            descriptions = baseDescriptions,
            iconKey = baseIconKey,
            dsp = baseDsp.copy(
                pitchSemitones = s.pitch.toDouble(),
                formantSemitones = s.formant.toDouble(),
                gainDb = s.gainDb.toDouble(),
                crispness = s.crispness,
                preserveFormants = s.preserveFormants,
                fx = s.effects,
            ),
            tags = baseTags,
        )
    }

    fun consumeMessage() = _state.update { it.copy(message = null) }

    private companion object {
        const val DEFAULT_ICON_KEY = "custom"
        const val CUSTOM_TAG = "custom"
        const val PROP_SETTLE_MS = 450L
    }
}
