package dev.avcrt.feature.presets

import android.content.Context
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import dagger.hilt.android.lifecycle.HiltViewModel
import dagger.hilt.android.qualifiers.ApplicationContext
import dev.avcrt.core.common.result.AvcrtResult
import dev.avcrt.core.common.result.userText
import dev.avcrt.domain.preset.Preset
import dev.avcrt.domain.preset.PresetOrigin
import dev.avcrt.domain.preset.PresetRepository
import dev.avcrt.domain.usecase.ApplyPresetUseCase
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
import java.util.UUID
import javax.inject.Inject

data class PresetsUiState(
    val builtIns: List<Preset> = emptyList(),
    val customs: List<Preset> = emptyList(),
    val activeId: String? = null,
    val transientMessage: String? = null,
    val applyingId: String? = null,
)

@HiltViewModel
class PresetsViewModel @Inject constructor(
    @ApplicationContext private val context: Context,
    private val presets: PresetRepository,
    private val applyPreset: ApplyPresetUseCase,
) : ViewModel() {

    private val transient = MutableStateFlow<String?>(null)
    private val applying = MutableStateFlow<String?>(null)

    /** One-shot: id of a preset to open in the editor (set right after a duplicate). */
    private val _editTarget = MutableStateFlow<String?>(null)
    val editTarget: StateFlow<String?> = _editTarget.asStateFlow()

    val state: StateFlow<PresetsUiState> = combine(
        presets.observeAll(),
        presets.observeActive(),
        transient,
        applying,
    ) { all, activeId, msg, applyId ->
        val (builtIns, customs) = all.partition { it.origin == PresetOrigin.BUILT_IN }
        PresetsUiState(
            builtIns = builtIns,
            customs = customs,
            activeId = activeId,
            transientMessage = msg,
            applyingId = applyId,
        )
    }.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5_000L), PresetsUiState())

    fun onApply(preset: Preset) {
        viewModelScope.launch {
            applying.value = preset.id
            when (val res = applyPreset(preset.id)) {
                is AvcrtResult.Success -> Unit
                is AvcrtResult.Failure -> transient.value = res.error.userText(context)
            }
            applying.value = null
        }
    }

    /** Copies a (usually built-in) preset into a new custom and opens it for editing. */
    fun onDuplicate(preset: Preset) {
        viewModelScope.launch {
            val copy = preset.copy(
                id = "custom.${UUID.randomUUID()}",
                origin = PresetOrigin.CUSTOM,
                tags = preset.tags.filterNot { it == "builtin" },
            )
            when (val res = presets.upsertCustom(copy)) {
                is AvcrtResult.Success -> _editTarget.value = copy.id
                is AvcrtResult.Failure -> transient.value = res.error.userText(context)
            }
        }
    }

    fun onDelete(id: String) {
        viewModelScope.launch {
            when (val res = presets.delete(id)) {
                is AvcrtResult.Success -> Unit
                is AvcrtResult.Failure -> transient.value = res.error.userText(context)
            }
        }
    }

    fun consumeEditTarget() {
        _editTarget.value = null
    }

    fun consumeTransient() {
        transient.value = null
    }
}
