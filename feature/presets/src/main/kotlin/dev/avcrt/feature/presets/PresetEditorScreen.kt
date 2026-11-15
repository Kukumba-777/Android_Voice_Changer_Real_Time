package dev.avcrt.feature.presets

import android.Manifest
import android.content.pm.PackageManager
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.rounded.ArrowBack
import androidx.compose.material.icons.rounded.DeleteOutline
import androidx.compose.material.icons.rounded.Mic
import androidx.compose.material.icons.rounded.PlayArrow
import androidx.compose.material.icons.rounded.Refresh
import androidx.compose.material.icons.rounded.Restore
import androidx.compose.material.icons.rounded.Stop
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.Checkbox
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.FilterChip
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Slider
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.core.content.ContextCompat
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.navigation.NavController
import dev.avcrt.core.ui.AvcrtSpacing
import dev.avcrt.core.ui.components.AvcrtScreen
import dev.avcrt.core.ui.components.SectionHeader
import dev.avcrt.domain.preset.EffectRange
import dev.avcrt.domain.preset.PresetDspRange
import dev.avcrt.domain.preset.PresetEffects
import kotlin.math.roundToInt

@Composable
fun PresetEditorScreen(
    navController: NavController,
    viewModel: PresetEditorViewModel = hiltViewModel(),
) {
    val state by viewModel.state.collectAsStateWithLifecycle()
    val snackbar = remember { SnackbarHostState() }

    LaunchedEffect(state.finished) {
        if (state.finished) navController.popBackStack()
    }
    LaunchedEffect(state.message) {
        state.message?.let {
            snackbar.showSnackbar(it)
            viewModel.consumeMessage()
        }
    }

    // Leaving the editor: hard-stop any in-app test record/playback and restore the real active
    // preset's config (the VM also does this in onCleared; this fires immediately on dispose).
    DisposableEffect(Unit) {
        onDispose { viewModel.onLeaveEditor() }
    }

    val title = stringResource(
        if (state.isEditing) R.string.presets_editor_edit_title
        else R.string.presets_editor_create_title,
    )

    AvcrtScreen(
        title = title,
        navigationIcon = {
            IconButton(onClick = { navController.popBackStack() }) {
                Icon(
                    Icons.AutoMirrored.Rounded.ArrowBack,
                    contentDescription = stringResource(R.string.presets_editor_back),
                )
            }
        },
    ) { _ ->
        Box(modifier = Modifier.fillMaxSize()) {
            // While an existing preset loads (edit mode) show a spinner instead of
            // the form, so the controls never flash their defaults before the saved
            // values arrive.
            if (state.loading) {
                CircularProgressIndicator(modifier = Modifier.align(Alignment.Center))
            } else {
                Column(modifier = Modifier.fillMaxSize()) {
                    // Scrollable form takes the remaining space; the Test + Save bar stays pinned
                    // below so both are reachable from anywhere without scrolling.
                    Box(modifier = Modifier.weight(1f)) {
                        PresetEditorForm(state = state, viewModel = viewModel)
                    }
                    EditorBottomBar(
                        state = state,
                        onStartTest = viewModel::onStartTest,
                        onPlayTest = viewModel::onPlayTest,
                        onStopTest = viewModel::onStopTest,
                        onSave = viewModel::onSave,
                    )
                }
            }
            SnackbarHost(hostState = snackbar, modifier = Modifier.align(Alignment.BottomCenter))
        }
    }
}

/** Pinned bottom bar: the in-app Test controls (record/listen) above a full-width Save button. */
@Composable
private fun EditorBottomBar(
    state: PresetEditorUiState,
    onStartTest: () -> Unit,
    onPlayTest: () -> Unit,
    onStopTest: () -> Unit,
    onSave: () -> Unit,
) {
    Surface(tonalElevation = 3.dp, shadowElevation = 8.dp) {
        Column(
            modifier = Modifier.padding(horizontal = AvcrtSpacing.lg, vertical = AvcrtSpacing.sm),
            verticalArrangement = Arrangement.spacedBy(AvcrtSpacing.sm),
        ) {
            PreviewTester(
                phase = state.previewPhase,
                recordElapsed = state.recordElapsed,
                canPlay = state.canPlayPreview,
                onStart = onStartTest,
                onPlay = onPlayTest,
                onStop = onStopTest,
            )
            Button(
                onClick = onSave,
                enabled = state.name.isNotBlank(),
                modifier = Modifier.fillMaxWidth(),
            ) {
                Text(stringResource(R.string.presets_editor_save))
            }
        }
    }
}

@Composable
private fun PresetEditorForm(
    state: PresetEditorUiState,
    viewModel: PresetEditorViewModel,
) {
    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(horizontal = AvcrtSpacing.lg),
        verticalArrangement = Arrangement.spacedBy(AvcrtSpacing.md),
    ) {
        Spacer(Modifier.height(AvcrtSpacing.sm))

        OutlinedTextField(
            value = state.name,
            onValueChange = viewModel::onName,
            label = { Text(stringResource(R.string.presets_editor_name)) },
            singleLine = true,
            isError = state.nameError,
            supportingText = if (state.nameError) {
                { Text(stringResource(R.string.presets_editor_name_required)) }
            } else {
                null
            },
            modifier = Modifier.fillMaxWidth(),
        )

        OutlinedTextField(
            value = state.description,
            onValueChange = viewModel::onDescription,
            label = { Text(stringResource(R.string.presets_editor_description)) },
            minLines = 1,
            maxLines = 3,
            modifier = Modifier.fillMaxWidth(),
        )

        SectionHeader(
            title = stringResource(R.string.presets_editor_section_voice),
            padding = PaddingValues(top = AvcrtSpacing.sm, bottom = AvcrtSpacing.xs),
        )

        DspSlider(
            label = stringResource(R.string.presets_pitch),
            value = state.pitch,
            onValueChange = viewModel::onPitch,
            valueRange = PresetDspRange.PITCH_MIN.toFloat()..PresetDspRange.PITCH_MAX.toFloat(),
            valueText = formatSemitones(state.pitch.toDouble()),
        )

        DspSlider(
            label = stringResource(R.string.presets_formant),
            value = state.formant,
            onValueChange = viewModel::onFormant,
            valueRange = PresetDspRange.FORMANT_MIN.toFloat()..PresetDspRange.FORMANT_MAX.toFloat(),
            valueText = formatSemitones(state.formant.toDouble()),
            hint = stringResource(R.string.presets_editor_formant_hint),
        )

        DspSlider(
            label = stringResource(R.string.presets_editor_gain),
            value = state.gainDb,
            onValueChange = viewModel::onGain,
            valueRange = PresetDspRange.GAIN_DB_MIN.toFloat()..PresetDspRange.GAIN_DB_MAX.toFloat(),
            valueText = formatDb(state.gainDb.toDouble()),
        )

        DspSlider(
            label = stringResource(R.string.presets_editor_crispness),
            value = state.crispness.toFloat(),
            onValueChange = { viewModel.onCrispness(it.roundToInt()) },
            valueRange = PresetDspRange.CRISPNESS_MIN.toFloat()..PresetDspRange.CRISPNESS_MAX.toFloat(),
            valueText = state.crispness.toString(),
            step = 1f,
            hint = stringResource(R.string.presets_editor_crispness_hint),
        )

        PreserveFormantsRow(
            checked = state.preserveFormants,
            onCheckedChange = viewModel::onPreserveFormants,
        )

        SectionHeader(
            title = stringResource(R.string.presets_fx_section),
            padding = PaddingValues(top = AvcrtSpacing.md, bottom = AvcrtSpacing.xs),
        )
        EffectsEditor(
            effects = state.effects,
            experimentalUnlocked = state.experimentalUnlocked,
            onUpdate = viewModel::updateEffects,
        )

        Row(verticalAlignment = Alignment.CenterVertically) {
            Checkbox(
                checked = state.applyAfterSave,
                onCheckedChange = viewModel::onApplyAfterSave,
            )
            Text(
                text = stringResource(R.string.presets_editor_apply_after),
                style = MaterialTheme.typography.bodyMedium,
            )
        }

        TextButton(onClick = viewModel::onReset) {
            Icon(Icons.Rounded.Restore, contentDescription = null)
            Spacer(Modifier.width(AvcrtSpacing.sm))
            Text(stringResource(R.string.presets_editor_reset))
        }

        if (state.isEditing) {
            DeleteSection(onConfirmed = viewModel::onDelete)
        }

        Spacer(Modifier.height(AvcrtSpacing.xxl))
    }
}

@Composable
private fun DeleteSection(onConfirmed: () -> Unit) {
    var confirm by remember { mutableStateOf(false) }
    OutlinedButton(
        onClick = { confirm = true },
        colors = ButtonDefaults.outlinedButtonColors(
            contentColor = MaterialTheme.colorScheme.error,
        ),
        modifier = Modifier.fillMaxWidth(),
    ) {
        Icon(Icons.Rounded.DeleteOutline, contentDescription = null)
        Spacer(Modifier.width(AvcrtSpacing.sm))
        Text(stringResource(R.string.presets_editor_delete))
    }
    if (confirm) {
        AlertDialog(
            onDismissRequest = { confirm = false },
            title = { Text(stringResource(R.string.presets_editor_delete_confirm_title)) },
            text = { Text(stringResource(R.string.presets_editor_delete_confirm_text)) },
            confirmButton = {
                TextButton(onClick = {
                    confirm = false
                    onConfirmed()
                }) { Text(stringResource(R.string.presets_delete)) }
            },
            dismissButton = {
                TextButton(onClick = { confirm = false }) {
                    Text(stringResource(R.string.presets_cancel))
                }
            },
        )
    }
}

@Composable
private fun DspSlider(
    label: String,
    value: Float,
    onValueChange: (Float) -> Unit,
    valueRange: ClosedFloatingPointRange<Float>,
    valueText: String,
    step: Float = 0.5f,
    hint: String? = null,
) {
    Column(modifier = Modifier.fillMaxWidth()) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(text = label, style = MaterialTheme.typography.titleSmall)
            Text(
                text = valueText,
                style = MaterialTheme.typography.labelLarge,
                color = MaterialTheme.colorScheme.primary,
            )
        }
        Slider(
            value = value,
            onValueChange = { onValueChange(roundToStep(it, step)) },
            valueRange = valueRange,
        )
        if (hint != null) {
            Text(
                text = hint,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

@Composable
private fun PreserveFormantsRow(checked: Boolean, onCheckedChange: (Boolean) -> Unit) {
    Row(
        modifier = Modifier.fillMaxWidth(),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(modifier = Modifier.weight(1f)) {
            Text(
                text = stringResource(R.string.presets_editor_preserve_formants),
                style = MaterialTheme.typography.titleSmall,
            )
            Text(
                text = stringResource(R.string.presets_editor_preserve_formants_hint),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        Spacer(Modifier.width(AvcrtSpacing.md))
        Switch(checked = checked, onCheckedChange = onCheckedChange)
    }
}

private fun roundToStep(value: Float, step: Float): Float =
    (value / step).roundToInt() * step

/**
 * In-app preset test: record a few seconds of the user's mic (voice-changed in-process by the Zygisk
 * hook while a preview preset is live) → play it back. Requests RECORD_AUDIO on first use.
 */
@Composable
private fun PreviewTester(
    phase: PreviewPhase,
    recordElapsed: Int,
    canPlay: Boolean,
    onStart: () -> Unit,
    onPlay: () -> Unit,
    onStop: () -> Unit,
) {
    val context = LocalContext.current
    val micPermission = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestPermission(),
    ) { granted -> if (granted) onStart() }
    val startWithPermission: () -> Unit = {
        if (ContextCompat.checkSelfPermission(context, Manifest.permission.RECORD_AUDIO) ==
            PackageManager.PERMISSION_GRANTED
        ) {
            onStart()
        } else {
            micPermission.launch(Manifest.permission.RECORD_AUDIO)
        }
    }

    Column(verticalArrangement = Arrangement.spacedBy(AvcrtSpacing.sm)) {
        when (phase) {
            PreviewPhase.Idle -> {
                Button(onClick = startWithPermission, modifier = Modifier.fillMaxWidth()) {
                    Icon(Icons.Rounded.Mic, contentDescription = null)
                    Spacer(Modifier.width(AvcrtSpacing.sm))
                    Text(stringResource(R.string.presets_editor_test_record))
                }
            }
            PreviewPhase.Recording -> {
                Text(
                    text = stringResource(
                        R.string.presets_editor_test_recording, recordElapsed, PREVIEW_RECORD_SECONDS,
                    ),
                    style = MaterialTheme.typography.titleSmall,
                )
                LinearProgressIndicator(
                    progress = { recordElapsed.toFloat() / PREVIEW_RECORD_SECONDS },
                    modifier = Modifier.fillMaxWidth(),
                )
                OutlinedButton(onClick = onStop, modifier = Modifier.fillMaxWidth()) {
                    Icon(Icons.Rounded.Stop, contentDescription = null)
                    Spacer(Modifier.width(AvcrtSpacing.sm))
                    Text(stringResource(R.string.presets_editor_test_stop))
                }
            }
            PreviewPhase.Recorded -> {
                // Both buttons weight(1f) (equal width) + single-line labels (equal height) so they
                // match in size.
                Row(horizontalArrangement = Arrangement.spacedBy(AvcrtSpacing.sm)) {
                    Button(
                        onClick = onPlay,
                        enabled = canPlay,
                        modifier = Modifier.weight(1f),
                    ) {
                        Icon(Icons.Rounded.PlayArrow, contentDescription = null)
                        Spacer(Modifier.width(AvcrtSpacing.sm))
                        Text(stringResource(R.string.presets_editor_test_listen), maxLines = 1)
                    }
                    OutlinedButton(onClick = startWithPermission, modifier = Modifier.weight(1f)) {
                        Icon(Icons.Rounded.Refresh, contentDescription = null)
                        Spacer(Modifier.width(AvcrtSpacing.sm))
                        Text(stringResource(R.string.presets_editor_test_again), maxLines = 1)
                    }
                }
            }
            PreviewPhase.Playing -> {
                OutlinedButton(onClick = onStop, modifier = Modifier.fillMaxWidth()) {
                    Icon(Icons.Rounded.Stop, contentDescription = null)
                    Spacer(Modifier.width(AvcrtSpacing.sm))
                    Text(stringResource(R.string.presets_editor_test_playing))
                }
            }
        }
    }
}

// ============================ Phase 6 effects editor ============================

@Composable
private fun EffectsEditor(
    effects: PresetEffects,
    experimentalUnlocked: Boolean,
    onUpdate: ((PresetEffects) -> PresetEffects) -> Unit,
) {
    Column(verticalArrangement = Arrangement.spacedBy(AvcrtSpacing.sm)) {
        // ---- Formant shift (Tier-3) — the headline natural male↔female ----
        EffectCard(
            title = stringResource(R.string.presets_fx_formant),
            enabled = effects.formant.enabled,
            onEnabledChange = { v -> onUpdate { it.copy(formant = it.formant.copy(enabled = v)) } },
        ) {
            DspSlider(stringResource(R.string.presets_fx_formant_shift), effects.formant.shift.toFloat(),
                { v -> onUpdate { it.copy(formant = it.formant.copy(shift = v.toDouble())) } },
                EffectRange.FORMANT_SHIFT_MIN.toFloat()..EffectRange.FORMANT_SHIFT_MAX.toFloat(),
                fmtX(effects.formant.shift.toFloat()), step = 0.05f)
        }

        // ---- Vocoder (Tier-3) ----
        EffectCard(
            title = stringResource(R.string.presets_fx_vocoder),
            enabled = effects.vocoder.enabled,
            onEnabledChange = { v -> onUpdate { it.copy(vocoder = it.vocoder.copy(enabled = v)) } },
        ) {
            DspSlider(stringResource(R.string.presets_fx_carrier), effects.vocoder.carrierHz.toFloat(),
                { v -> onUpdate { it.copy(vocoder = it.vocoder.copy(carrierHz = v.toDouble())) } },
                EffectRange.VOCODER_CARRIER_MIN.toFloat()..EffectRange.VOCODER_CARRIER_MAX.toFloat(),
                fmtHz(effects.vocoder.carrierHz.toFloat()), step = 1f)
            CarrierSelector(effects.vocoder.carrierType) { t ->
                onUpdate { it.copy(vocoder = it.vocoder.copy(carrierType = t)) }
            }
            DspSlider(stringResource(R.string.presets_fx_mix), effects.vocoder.mix.toFloat(),
                { v -> onUpdate { it.copy(vocoder = it.vocoder.copy(mix = v.toDouble())) } },
                unit, fmtPct(effects.vocoder.mix.toFloat()), step = 0.01f)
        }

        // ---- Whisper (Tier-3) ----
        EffectCard(
            title = stringResource(R.string.presets_fx_whisper),
            enabled = effects.whisper.enabled,
            onEnabledChange = { v -> onUpdate { it.copy(whisper = it.whisper.copy(enabled = v)) } },
        ) {
            DspSlider(stringResource(R.string.presets_fx_mix), effects.whisper.mix.toFloat(),
                { v -> onUpdate { it.copy(whisper = it.whisper.copy(mix = v.toDouble())) } },
                unit, fmtPct(effects.whisper.mix.toFloat()), step = 0.01f)
        }

        // ---- Reverb ----
        EffectCard(
            title = stringResource(R.string.presets_fx_reverb),
            enabled = effects.reverb.enabled,
            onEnabledChange = { v -> onUpdate { it.copy(reverb = it.reverb.copy(enabled = v)) } },
        ) {
            DspSlider(stringResource(R.string.presets_fx_room), effects.reverb.roomSize.toFloat(),
                { v -> onUpdate { it.copy(reverb = it.reverb.copy(roomSize = v.toDouble())) } },
                unit, fmtPct(effects.reverb.roomSize.toFloat()), step = 0.01f)
            DspSlider(stringResource(R.string.presets_fx_damping), effects.reverb.damping.toFloat(),
                { v -> onUpdate { it.copy(reverb = it.reverb.copy(damping = v.toDouble())) } },
                unit, fmtPct(effects.reverb.damping.toFloat()), step = 0.01f)
            DspSlider(stringResource(R.string.presets_fx_wet), effects.reverb.wet.toFloat(),
                { v -> onUpdate { it.copy(reverb = it.reverb.copy(wet = v.toDouble())) } },
                unit, fmtPct(effects.reverb.wet.toFloat()), step = 0.01f)
        }

        // ---- Equalizer ----
        EffectCard(
            title = stringResource(R.string.presets_fx_eq),
            enabled = effects.eq.enabled,
            onEnabledChange = { v -> onUpdate { it.copy(eq = it.eq.copy(enabled = v)) } },
        ) {
            val r = EffectRange.EQ_DB_MIN.toFloat()..EffectRange.EQ_DB_MAX.toFloat()
            DspSlider(stringResource(R.string.presets_fx_low), effects.eq.lowDb.toFloat(),
                { v -> onUpdate { it.copy(eq = it.eq.copy(lowDb = v.toDouble())) } }, r,
                fmtDb(effects.eq.lowDb.toFloat()), step = 0.5f)
            DspSlider(stringResource(R.string.presets_fx_mid), effects.eq.midDb.toFloat(),
                { v -> onUpdate { it.copy(eq = it.eq.copy(midDb = v.toDouble())) } }, r,
                fmtDb(effects.eq.midDb.toFloat()), step = 0.5f)
            DspSlider(stringResource(R.string.presets_fx_high), effects.eq.highDb.toFloat(),
                { v -> onUpdate { it.copy(eq = it.eq.copy(highDb = v.toDouble())) } }, r,
                fmtDb(effects.eq.highDb.toFloat()), step = 0.5f)
        }

        // ---- Telephone (toggle only) ----
        EffectCard(
            title = stringResource(R.string.presets_fx_telephone),
            enabled = effects.telephone.enabled,
            onEnabledChange = { v -> onUpdate { it.copy(telephone = it.telephone.copy(enabled = v)) } },
        )

        // ---- Ring modulator (robot) ----
        EffectCard(
            title = stringResource(R.string.presets_fx_ringmod),
            enabled = effects.ringmod.enabled,
            onEnabledChange = { v -> onUpdate { it.copy(ringmod = it.ringmod.copy(enabled = v)) } },
        ) {
            DspSlider(stringResource(R.string.presets_fx_carrier), effects.ringmod.carrierHz.toFloat(),
                { v -> onUpdate { it.copy(ringmod = it.ringmod.copy(carrierHz = v.toDouble())) } },
                EffectRange.RING_CARRIER_MIN.toFloat()..EffectRange.RING_CARRIER_MAX.toFloat(),
                fmtHz(effects.ringmod.carrierHz.toFloat()), step = 1f)
            DspSlider(stringResource(R.string.presets_fx_mix), effects.ringmod.mix.toFloat(),
                { v -> onUpdate { it.copy(ringmod = it.ringmod.copy(mix = v.toDouble())) } },
                unit, fmtPct(effects.ringmod.mix.toFloat()), step = 0.01f)
        }

        // ---- Tremolo (fan) ----
        EffectCard(
            title = stringResource(R.string.presets_fx_tremolo),
            enabled = effects.tremolo.enabled,
            onEnabledChange = { v -> onUpdate { it.copy(tremolo = it.tremolo.copy(enabled = v)) } },
        ) {
            DspSlider(stringResource(R.string.presets_fx_rate), effects.tremolo.rateHz.toFloat(),
                { v -> onUpdate { it.copy(tremolo = it.tremolo.copy(rateHz = v.toDouble())) } },
                EffectRange.TREM_RATE_MIN.toFloat()..EffectRange.TREM_RATE_MAX.toFloat(),
                fmtHz(effects.tremolo.rateHz.toFloat()), step = 0.1f)
            DspSlider(stringResource(R.string.presets_fx_depth), effects.tremolo.depth.toFloat(),
                { v -> onUpdate { it.copy(tremolo = it.tremolo.copy(depth = v.toDouble())) } },
                unit, fmtPct(effects.tremolo.depth.toFloat()), step = 0.01f)
            ShapeSelector(effects.tremolo.shape) { s ->
                onUpdate { it.copy(tremolo = it.tremolo.copy(shape = s)) }
            }
        }

        // ---- Distortion ----
        EffectCard(
            title = stringResource(R.string.presets_fx_distortion),
            enabled = effects.distortion.enabled,
            onEnabledChange = { v -> onUpdate { it.copy(distortion = it.distortion.copy(enabled = v)) } },
        ) {
            DspSlider(stringResource(R.string.presets_fx_drive), effects.distortion.drive.toFloat(),
                { v -> onUpdate { it.copy(distortion = it.distortion.copy(drive = v.toDouble())) } },
                EffectRange.DIST_DRIVE_MIN.toFloat()..EffectRange.DIST_DRIVE_MAX.toFloat(),
                fmtDrive(effects.distortion.drive.toFloat()), step = 0.5f)
            DspSlider(stringResource(R.string.presets_fx_mix), effects.distortion.mix.toFloat(),
                { v -> onUpdate { it.copy(distortion = it.distortion.copy(mix = v.toDouble())) } },
                unit, fmtPct(effects.distortion.mix.toFloat()), step = 0.01f)
        }

        // ---- Echo ----
        EffectCard(
            title = stringResource(R.string.presets_fx_echo),
            enabled = effects.echo.enabled,
            onEnabledChange = { v -> onUpdate { it.copy(echo = it.echo.copy(enabled = v)) } },
        ) {
            DspSlider(stringResource(R.string.presets_fx_time), effects.echo.timeMs.toFloat(),
                { v -> onUpdate { it.copy(echo = it.echo.copy(timeMs = v.toDouble())) } },
                EffectRange.ECHO_TIME_MIN.toFloat()..EffectRange.ECHO_TIME_MAX.toFloat(),
                fmtMs(effects.echo.timeMs.toFloat()), step = 1f)
            DspSlider(stringResource(R.string.presets_fx_feedback), effects.echo.feedback.toFloat(),
                { v -> onUpdate { it.copy(echo = it.echo.copy(feedback = v.toDouble())) } },
                EffectRange.ECHO_FB_MIN.toFloat()..EffectRange.ECHO_FB_MAX.toFloat(),
                fmtPct(effects.echo.feedback.toFloat()), step = 0.01f)
            DspSlider(stringResource(R.string.presets_fx_mix), effects.echo.mix.toFloat(),
                { v -> onUpdate { it.copy(echo = it.echo.copy(mix = v.toDouble())) } },
                unit, fmtPct(effects.echo.mix.toFloat()), step = 0.01f)
        }

        // ---- Filter (sweepable HP/LP) ----
        EffectCard(
            title = stringResource(R.string.presets_fx_filter),
            enabled = effects.filter.enabled,
            onEnabledChange = { v -> onUpdate { it.copy(filter = it.filter.copy(enabled = v)) } },
        ) {
            DspSlider(stringResource(R.string.presets_fx_hp), effects.filter.hpHz.toFloat(),
                { v -> onUpdate { it.copy(filter = it.filter.copy(hpHz = v.toDouble())) } },
                EffectRange.FILTER_HP_MIN.toFloat()..EffectRange.FILTER_HP_MAX.toFloat(),
                fmtCutoff(effects.filter.hpHz.toFloat()), step = 10f)
            DspSlider(stringResource(R.string.presets_fx_lp), effects.filter.lpHz.toFloat(),
                { v -> onUpdate { it.copy(filter = it.filter.copy(lpHz = v.toDouble())) } },
                EffectRange.FILTER_LP_MIN.toFloat()..EffectRange.FILTER_LP_MAX.toFloat(),
                fmtCutoff(effects.filter.lpHz.toFloat()), step = 50f)
            DspSlider(stringResource(R.string.presets_fx_resonance), effects.filter.q.toFloat(),
                { v -> onUpdate { it.copy(filter = it.filter.copy(q = v.toDouble())) } },
                EffectRange.FILTER_Q_MIN.toFloat()..EffectRange.FILTER_Q_MAX.toFloat(),
                fmtQ(effects.filter.q.toFloat()), step = 0.1f)
        }

        // ---- Vibrato ----
        EffectCard(
            title = stringResource(R.string.presets_fx_vibrato),
            enabled = effects.vibrato.enabled,
            onEnabledChange = { v -> onUpdate { it.copy(vibrato = it.vibrato.copy(enabled = v)) } },
        ) {
            DspSlider(stringResource(R.string.presets_fx_rate), effects.vibrato.rateHz.toFloat(),
                { v -> onUpdate { it.copy(vibrato = it.vibrato.copy(rateHz = v.toDouble())) } },
                EffectRange.VIB_RATE_MIN.toFloat()..EffectRange.VIB_RATE_MAX.toFloat(),
                fmtHz(effects.vibrato.rateHz.toFloat()), step = 0.1f)
            DspSlider(stringResource(R.string.presets_fx_depth), effects.vibrato.depth.toFloat(),
                { v -> onUpdate { it.copy(vibrato = it.vibrato.copy(depth = v.toDouble())) } },
                unit, fmtPct(effects.vibrato.depth.toFloat()), step = 0.01f)
        }

        // ---- Chorus ----
        EffectCard(
            title = stringResource(R.string.presets_fx_chorus),
            enabled = effects.chorus.enabled,
            onEnabledChange = { v -> onUpdate { it.copy(chorus = it.chorus.copy(enabled = v)) } },
        ) {
            DspSlider(stringResource(R.string.presets_fx_rate), effects.chorus.rateHz.toFloat(),
                { v -> onUpdate { it.copy(chorus = it.chorus.copy(rateHz = v.toDouble())) } },
                EffectRange.CHOR_RATE_MIN.toFloat()..EffectRange.CHOR_RATE_MAX.toFloat(),
                fmtHz(effects.chorus.rateHz.toFloat()), step = 0.1f)
            DspSlider(stringResource(R.string.presets_fx_depth), effects.chorus.depth.toFloat(),
                { v -> onUpdate { it.copy(chorus = it.chorus.copy(depth = v.toDouble())) } },
                unit, fmtPct(effects.chorus.depth.toFloat()), step = 0.01f)
            DspSlider(stringResource(R.string.presets_fx_mix), effects.chorus.mix.toFloat(),
                { v -> onUpdate { it.copy(chorus = it.chorus.copy(mix = v.toDouble())) } },
                unit, fmtPct(effects.chorus.mix.toFloat()), step = 0.01f)
        }

        // ---- Flanger ----
        EffectCard(
            title = stringResource(R.string.presets_fx_flanger),
            enabled = effects.flanger.enabled,
            onEnabledChange = { v -> onUpdate { it.copy(flanger = it.flanger.copy(enabled = v)) } },
        ) {
            DspSlider(stringResource(R.string.presets_fx_rate), effects.flanger.rateHz.toFloat(),
                { v -> onUpdate { it.copy(flanger = it.flanger.copy(rateHz = v.toDouble())) } },
                EffectRange.FLAN_RATE_MIN.toFloat()..EffectRange.FLAN_RATE_MAX.toFloat(),
                fmtHz(effects.flanger.rateHz.toFloat()), step = 0.1f)
            DspSlider(stringResource(R.string.presets_fx_depth), effects.flanger.depth.toFloat(),
                { v -> onUpdate { it.copy(flanger = it.flanger.copy(depth = v.toDouble())) } },
                unit, fmtPct(effects.flanger.depth.toFloat()), step = 0.01f)
            DspSlider(stringResource(R.string.presets_fx_feedback), effects.flanger.feedback.toFloat(),
                { v -> onUpdate { it.copy(flanger = it.flanger.copy(feedback = v.toDouble())) } },
                EffectRange.FLAN_FB_MIN.toFloat()..EffectRange.FLAN_FB_MAX.toFloat(),
                fmtPct(effects.flanger.feedback.toFloat()), step = 0.01f)
            DspSlider(stringResource(R.string.presets_fx_mix), effects.flanger.mix.toFloat(),
                { v -> onUpdate { it.copy(flanger = it.flanger.copy(mix = v.toDouble())) } },
                unit, fmtPct(effects.flanger.mix.toFloat()), step = 0.01f)
        }

        // ---- Phaser ----
        EffectCard(
            title = stringResource(R.string.presets_fx_phaser),
            enabled = effects.phaser.enabled,
            onEnabledChange = { v -> onUpdate { it.copy(phaser = it.phaser.copy(enabled = v)) } },
        ) {
            DspSlider(stringResource(R.string.presets_fx_rate), effects.phaser.rateHz.toFloat(),
                { v -> onUpdate { it.copy(phaser = it.phaser.copy(rateHz = v.toDouble())) } },
                EffectRange.PHAS_RATE_MIN.toFloat()..EffectRange.PHAS_RATE_MAX.toFloat(),
                fmtHz(effects.phaser.rateHz.toFloat()), step = 0.1f)
            DspSlider(stringResource(R.string.presets_fx_depth), effects.phaser.depth.toFloat(),
                { v -> onUpdate { it.copy(phaser = it.phaser.copy(depth = v.toDouble())) } },
                unit, fmtPct(effects.phaser.depth.toFloat()), step = 0.01f)
            DspSlider(stringResource(R.string.presets_fx_feedback), effects.phaser.feedback.toFloat(),
                { v -> onUpdate { it.copy(phaser = it.phaser.copy(feedback = v.toDouble())) } },
                EffectRange.PHAS_FB_MIN.toFloat()..EffectRange.PHAS_FB_MAX.toFloat(),
                fmtPct(effects.phaser.feedback.toFloat()), step = 0.01f)
            DspSlider(stringResource(R.string.presets_fx_mix), effects.phaser.mix.toFloat(),
                { v -> onUpdate { it.copy(phaser = it.phaser.copy(mix = v.toDouble())) } },
                unit, fmtPct(effects.phaser.mix.toFloat()), step = 0.01f)
        }

        // ---- Auto-wah ----
        EffectCard(
            title = stringResource(R.string.presets_fx_autowah),
            enabled = effects.autowah.enabled,
            onEnabledChange = { v -> onUpdate { it.copy(autowah = it.autowah.copy(enabled = v)) } },
        ) {
            DspSlider(stringResource(R.string.presets_fx_sensitivity), effects.autowah.sensitivity.toFloat(),
                { v -> onUpdate { it.copy(autowah = it.autowah.copy(sensitivity = v.toDouble())) } },
                unit, fmtPct(effects.autowah.sensitivity.toFloat()), step = 0.01f)
            DspSlider(stringResource(R.string.presets_fx_base_freq), effects.autowah.baseHz.toFloat(),
                { v -> onUpdate { it.copy(autowah = it.autowah.copy(baseHz = v.toDouble())) } },
                EffectRange.WAH_BASE_MIN.toFloat()..EffectRange.WAH_BASE_MAX.toFloat(),
                fmtHz(effects.autowah.baseHz.toFloat()), step = 10f)
            DspSlider(stringResource(R.string.presets_fx_range), effects.autowah.rangeHz.toFloat(),
                { v -> onUpdate { it.copy(autowah = it.autowah.copy(rangeHz = v.toDouble())) } },
                EffectRange.WAH_RANGE_MIN.toFloat()..EffectRange.WAH_RANGE_MAX.toFloat(),
                fmtHz(effects.autowah.rangeHz.toFloat()), step = 50f)
            DspSlider(stringResource(R.string.presets_fx_mix), effects.autowah.mix.toFloat(),
                { v -> onUpdate { it.copy(autowah = it.autowah.copy(mix = v.toDouble())) } },
                unit, fmtPct(effects.autowah.mix.toFloat()), step = 0.01f)
        }

        // ---- Bitcrusher ----
        EffectCard(
            title = stringResource(R.string.presets_fx_bitcrush),
            enabled = effects.bitcrush.enabled,
            onEnabledChange = { v -> onUpdate { it.copy(bitcrush = it.bitcrush.copy(enabled = v)) } },
        ) {
            DspSlider(stringResource(R.string.presets_fx_bits), effects.bitcrush.bits.toFloat(),
                { v -> onUpdate { it.copy(bitcrush = it.bitcrush.copy(bits = v.toDouble())) } },
                EffectRange.CRUSH_BITS_MIN.toFloat()..EffectRange.CRUSH_BITS_MAX.toFloat(),
                fmtBits(effects.bitcrush.bits.toFloat()), step = 1f)
            DspSlider(stringResource(R.string.presets_fx_downsample), effects.bitcrush.downsample.toFloat(),
                { v -> onUpdate { it.copy(bitcrush = it.bitcrush.copy(downsample = v.toDouble())) } },
                EffectRange.CRUSH_DS_MIN.toFloat()..EffectRange.CRUSH_DS_MAX.toFloat(),
                fmtDs(effects.bitcrush.downsample.toFloat()), step = 1f)
            DspSlider(stringResource(R.string.presets_fx_mix), effects.bitcrush.mix.toFloat(),
                { v -> onUpdate { it.copy(bitcrush = it.bitcrush.copy(mix = v.toDouble())) } },
                unit, fmtPct(effects.bitcrush.mix.toFloat()), step = 0.01f)
        }

        // ---- Compressor ----
        EffectCard(
            title = stringResource(R.string.presets_fx_compressor),
            enabled = effects.compressor.enabled,
            onEnabledChange = { v -> onUpdate { it.copy(compressor = it.compressor.copy(enabled = v)) } },
        ) {
            DspSlider(stringResource(R.string.presets_fx_threshold), effects.compressor.thresholdDb.toFloat(),
                { v -> onUpdate { it.copy(compressor = it.compressor.copy(thresholdDb = v.toDouble())) } },
                EffectRange.COMP_THRESH_MIN.toFloat()..EffectRange.COMP_THRESH_MAX.toFloat(),
                fmtDb(effects.compressor.thresholdDb.toFloat()), step = 1f)
            DspSlider(stringResource(R.string.presets_fx_ratio), effects.compressor.ratio.toFloat(),
                { v -> onUpdate { it.copy(compressor = it.compressor.copy(ratio = v.toDouble())) } },
                EffectRange.COMP_RATIO_MIN.toFloat()..EffectRange.COMP_RATIO_MAX.toFloat(),
                fmtRatio(effects.compressor.ratio.toFloat()), step = 0.5f)
            DspSlider(stringResource(R.string.presets_fx_attack), effects.compressor.attackMs.toFloat(),
                { v -> onUpdate { it.copy(compressor = it.compressor.copy(attackMs = v.toDouble())) } },
                EffectRange.COMP_ATTACK_MIN.toFloat()..EffectRange.COMP_ATTACK_MAX.toFloat(),
                fmtMs(effects.compressor.attackMs.toFloat()), step = 0.5f)
            DspSlider(stringResource(R.string.presets_fx_release), effects.compressor.releaseMs.toFloat(),
                { v -> onUpdate { it.copy(compressor = it.compressor.copy(releaseMs = v.toDouble())) } },
                EffectRange.COMP_RELEASE_MIN.toFloat()..EffectRange.COMP_RELEASE_MAX.toFloat(),
                fmtMs(effects.compressor.releaseMs.toFloat()), step = 5f)
            DspSlider(stringResource(R.string.presets_fx_makeup), effects.compressor.makeupDb.toFloat(),
                { v -> onUpdate { it.copy(compressor = it.compressor.copy(makeupDb = v.toDouble())) } },
                EffectRange.COMP_MAKEUP_MIN.toFloat()..EffectRange.COMP_MAKEUP_MAX.toFloat(),
                fmtDb(effects.compressor.makeupDb.toFloat()), step = 0.5f)
        }

        // ---- Noise gate (utility — kept last) ----
        EffectCard(
            title = stringResource(R.string.presets_fx_gate),
            enabled = effects.gate.enabled,
            onEnabledChange = { v -> onUpdate { it.copy(gate = it.gate.copy(enabled = v)) } },
        ) {
            DspSlider(
                stringResource(R.string.presets_fx_threshold), effects.gate.thresholdDb.toFloat(),
                { v -> onUpdate { it.copy(gate = it.gate.copy(thresholdDb = v.toDouble())) } },
                EffectRange.GATE_THRESH_MIN.toFloat()..EffectRange.GATE_THRESH_MAX.toFloat(),
                fmtDb(effects.gate.thresholdDb.toFloat()), step = 1f,
            )
            DspSlider(
                stringResource(R.string.presets_fx_attack), effects.gate.attackMs.toFloat(),
                { v -> onUpdate { it.copy(gate = it.gate.copy(attackMs = v.toDouble())) } },
                EffectRange.GATE_ATTACK_MIN.toFloat()..EffectRange.GATE_ATTACK_MAX.toFloat(),
                fmtMs(effects.gate.attackMs.toFloat()), step = 0.5f,
            )
            DspSlider(
                stringResource(R.string.presets_fx_release), effects.gate.releaseMs.toFloat(),
                { v -> onUpdate { it.copy(gate = it.gate.copy(releaseMs = v.toDouble())) } },
                EffectRange.GATE_RELEASE_MIN.toFloat()..EffectRange.GATE_RELEASE_MAX.toFloat(),
                fmtMs(effects.gate.releaseMs.toFloat()), step = 1f,
            )
        }

        // ---- Experimental (hidden behind Settings → Experimental) ----
        if (experimentalUnlocked) {
            SectionHeader(
                title = stringResource(R.string.presets_fx_experimental_section),
                padding = PaddingValues(top = AvcrtSpacing.md, bottom = AvcrtSpacing.xs),
            )
            Text(
                text = stringResource(R.string.presets_fx_experimental_hint),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            // Spectral (FFT). Auto-tune / harmonizer land here in a later stage.
            EffectCard(
                title = stringResource(R.string.presets_fx_spec_robot),
                enabled = effects.spectral.robot,
                onEnabledChange = { v -> onUpdate { it.copy(spectral = it.spectral.copy(robot = v)) } },
            )
            EffectCard(
                title = stringResource(R.string.presets_fx_spec_whisper),
                enabled = effects.spectral.whisper,
                onEnabledChange = { v -> onUpdate { it.copy(spectral = it.spectral.copy(whisper = v)) } },
            )
            EffectCard(
                title = stringResource(R.string.presets_fx_spec_freeze),
                enabled = effects.spectral.freeze,
                onEnabledChange = { v -> onUpdate { it.copy(spectral = it.spectral.copy(freeze = v)) } },
            )
            EffectCard(
                title = stringResource(R.string.presets_fx_spec_blur),
                enabled = effects.spectral.blur,
                onEnabledChange = { v -> onUpdate { it.copy(spectral = it.spectral.copy(blur = v)) } },
            ) {
                DspSlider(stringResource(R.string.presets_fx_depth), effects.spectral.blurAmt.toFloat(),
                    { v -> onUpdate { it.copy(spectral = it.spectral.copy(blurAmt = v.toDouble())) } },
                    unit, fmtPct(effects.spectral.blurAmt.toFloat()), step = 0.01f)
            }
            // Shared wet/dry strength for the spectral ops above (robot/whisper/freeze/blur).
            DspSlider(stringResource(R.string.presets_fx_spec_strength), effects.spectral.mix.toFloat(),
                { v -> onUpdate { it.copy(spectral = it.spectral.copy(mix = v.toDouble())) } },
                unit, fmtPct(effects.spectral.mix.toFloat()), step = 0.01f)

            // ---- Auto-tune (phase vocoder) ----
            EffectCard(
                title = stringResource(R.string.presets_fx_autotune),
                enabled = effects.autotune.enabled,
                onEnabledChange = { v -> onUpdate { it.copy(autotune = it.autotune.copy(enabled = v)) } },
            ) {
                DspSlider(stringResource(R.string.presets_fx_strength), effects.autotune.strength.toFloat(),
                    { v -> onUpdate { it.copy(autotune = it.autotune.copy(strength = v.toDouble())) } },
                    unit, fmtPct(effects.autotune.strength.toFloat()), step = 0.01f)
            }

            // ---- Harmonizer (phase vocoder, 2 voices) ----
            EffectCard(
                title = stringResource(R.string.presets_fx_harmonizer),
                enabled = effects.harmonizer.enabled,
                onEnabledChange = { v -> onUpdate { it.copy(harmonizer = it.harmonizer.copy(enabled = v)) } },
            ) {
                val ir = EffectRange.HARMON_INTERVAL_MIN.toFloat()..EffectRange.HARMON_INTERVAL_MAX.toFloat()
                DspSlider(stringResource(R.string.presets_fx_interval1), effects.harmonizer.interval1.toFloat(),
                    { v -> onUpdate { it.copy(harmonizer = it.harmonizer.copy(interval1 = v.roundToInt())) } },
                    ir, fmtSemi(effects.harmonizer.interval1), step = 1f)
                DspSlider(stringResource(R.string.presets_fx_interval2), effects.harmonizer.interval2.toFloat(),
                    { v -> onUpdate { it.copy(harmonizer = it.harmonizer.copy(interval2 = v.roundToInt())) } },
                    ir, fmtSemi(effects.harmonizer.interval2), step = 1f)
                DspSlider(stringResource(R.string.presets_fx_mix), effects.harmonizer.mix.toFloat(),
                    { v -> onUpdate { it.copy(harmonizer = it.harmonizer.copy(mix = v.toDouble())) } },
                    unit, fmtPct(effects.harmonizer.mix.toFloat()), step = 0.01f)
            }
        }
    }
}

private val unit = 0f..1f

@Composable
private fun EffectCard(
    title: String,
    enabled: Boolean,
    onEnabledChange: (Boolean) -> Unit,
    content: @Composable () -> Unit = {},
) {
    Card(
        modifier = Modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceVariant),
    ) {
        Column(modifier = Modifier.padding(AvcrtSpacing.md)) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text(text = title, style = MaterialTheme.typography.titleSmall)
                Switch(checked = enabled, onCheckedChange = onEnabledChange)
            }
            if (enabled) {
                Spacer(Modifier.height(AvcrtSpacing.xs))
                content()
            }
        }
    }
}

@Composable
private fun ShapeSelector(shape: Int, onShape: (Int) -> Unit) {
    val labels = listOf(
        0 to stringResource(R.string.presets_fx_shape_sine),
        1 to stringResource(R.string.presets_fx_shape_tri),
        2 to stringResource(R.string.presets_fx_shape_square),
    )
    Column {
        Text(
            text = stringResource(R.string.presets_fx_shape),
            style = MaterialTheme.typography.titleSmall,
        )
        Spacer(Modifier.height(AvcrtSpacing.xs))
        Row(horizontalArrangement = Arrangement.spacedBy(AvcrtSpacing.sm)) {
            labels.forEach { (s, lbl) ->
                FilterChip(selected = shape == s, onClick = { onShape(s) }, label = { Text(lbl) })
            }
        }
    }
}

@Composable
private fun CarrierSelector(type: Int, onType: (Int) -> Unit) {
    val labels = listOf(
        0 to stringResource(R.string.presets_fx_carrier_saw),
        1 to stringResource(R.string.presets_fx_carrier_pulse),
        2 to stringResource(R.string.presets_fx_carrier_noise),
    )
    Column {
        Text(
            text = stringResource(R.string.presets_fx_carrier_type),
            style = MaterialTheme.typography.titleSmall,
        )
        Spacer(Modifier.height(AvcrtSpacing.xs))
        Row(horizontalArrangement = Arrangement.spacedBy(AvcrtSpacing.sm)) {
            labels.forEach { (t, lbl) ->
                FilterChip(selected = type == t, onClick = { onType(t) }, label = { Text(lbl) })
            }
        }
    }
}

private fun fmtHz(v: Float): String = "%.0f Hz".format(v)
private fun fmtX(v: Float): String = "%.2f×".format(v)
private fun fmtSemi(v: Int): String = (if (v > 0) "+" else "") + "$v st"
private fun fmtMs(v: Float): String = "%.0f ms".format(v)
private fun fmtPct(v: Float): String = "%.0f%%".format(v * 100f)
private fun fmtDrive(v: Float): String = "%.1f×".format(v)
private fun fmtDb(v: Float): String = (if (v > 0f) "+" else "") + "%.1f dB".format(v)
private fun fmtQ(v: Float): String = "%.1f".format(v)
private fun fmtBits(v: Float): String = "%.0f bit".format(v)
private fun fmtDs(v: Float): String = "%.0f×".format(v)
private fun fmtRatio(v: Float): String = "%.1f:1".format(v)
private fun fmtCutoff(v: Float): String = if (v < 20f) "—" else "%.0f Hz".format(v)
