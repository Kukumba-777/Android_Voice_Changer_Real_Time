package dev.avcrt.feature.converter

import android.Manifest
import android.content.pm.PackageManager
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.FilterChip
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Slider
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.core.content.ContextCompat
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import dev.avcrt.core.ui.AvcrtSpacing
import dev.avcrt.core.ui.components.AvcrtScreen
import kotlin.math.roundToInt

@Composable
fun ConverterScreen(
    viewModel: ConverterViewModel = hiltViewModel(),
) {
    val state by viewModel.state.collectAsStateWithLifecycle()
    val snackbar = remember { SnackbarHostState() }
    val context = LocalContext.current

    val permissionLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestPermission(),
    ) { granted -> if (granted) viewModel.record() else viewModel.onPermissionDenied() }

    val livePermissionLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestPermission(),
    ) { granted -> if (granted) viewModel.toggleLive() else viewModel.onPermissionDenied() }

    LaunchedEffect(state.message) {
        state.message?.let {
            snackbar.showSnackbar(it)
            viewModel.consumeMessage()
        }
    }

    val startRecording = {
        val granted = ContextCompat.checkSelfPermission(context, Manifest.permission.RECORD_AUDIO) ==
            PackageManager.PERMISSION_GRANTED
        if (granted) viewModel.record() else permissionLauncher.launch(Manifest.permission.RECORD_AUDIO)
    }

    val startLive = {
        val granted = ContextCompat.checkSelfPermission(context, Manifest.permission.RECORD_AUDIO) ==
            PackageManager.PERMISSION_GRANTED
        if (granted) viewModel.toggleLive() else livePermissionLauncher.launch(Manifest.permission.RECORD_AUDIO)
    }

    AvcrtScreen(title = stringResource(R.string.converter_title)) { _ ->
        Box(Modifier.fillMaxSize()) {
            Column(
                modifier = Modifier
                    .fillMaxSize()
                    .verticalScroll(rememberScrollState())
                    .padding(AvcrtSpacing.lg),
                verticalArrangement = Arrangement.spacedBy(AvcrtSpacing.lg),
            ) {
                val idle = state.phase == ConverterPhase.IDLE

                // 1 — models
                Card(Modifier.fillMaxWidth()) {
                    Column(
                        Modifier.padding(AvcrtSpacing.lg),
                        verticalArrangement = Arrangement.spacedBy(AvcrtSpacing.sm),
                    ) {
                        Text(stringResource(R.string.converter_models_header), style = MaterialTheme.typography.titleMedium)
                        if (!state.sharedReady) {
                            Text(
                                stringResource(R.string.converter_models_missing),
                                style = MaterialTheme.typography.bodySmall,
                                color = MaterialTheme.colorScheme.error,
                            )
                        }
                        Text(
                            stringResource(R.string.converter_models_dir, state.modelsDir),
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                        if (state.voices.isEmpty()) {
                            Text(stringResource(R.string.converter_no_voices), style = MaterialTheme.typography.bodyMedium)
                        } else {
                            state.voices.forEach { v ->
                                Row(
                                    modifier = Modifier
                                        .fillMaxWidth()
                                        .selectable(
                                            selected = state.selectedVoiceId == v.id,
                                            onClick = { viewModel.selectVoice(v.id) },
                                        )
                                        .padding(vertical = AvcrtSpacing.xs),
                                    verticalAlignment = Alignment.CenterVertically,
                                ) {
                                    RadioButton(
                                        selected = state.selectedVoiceId == v.id,
                                        onClick = { viewModel.selectVoice(v.id) },
                                    )
                                    Spacer(Modifier.width(AvcrtSpacing.sm))
                                    Text(v.name, maxLines = 1, overflow = TextOverflow.Ellipsis)
                                }
                            }
                        }
                        OutlinedButton(onClick = { viewModel.scan() }) {
                            Text(stringResource(R.string.converter_scan))
                        }
                    }
                }

                // 2 — record your voice
                Card(Modifier.fillMaxWidth()) {
                    Column(
                        Modifier.padding(AvcrtSpacing.lg),
                        verticalArrangement = Arrangement.spacedBy(AvcrtSpacing.sm),
                    ) {
                        Text(stringResource(R.string.converter_record_header), style = MaterialTheme.typography.titleMedium)
                        Row(horizontalArrangement = Arrangement.spacedBy(AvcrtSpacing.sm)) {
                            if (state.phase == ConverterPhase.RECORDING) {
                                Button(onClick = { viewModel.stopRecord() }) {
                                    Text(stringResource(R.string.converter_stop))
                                }
                            } else {
                                Button(onClick = { startRecording() }, enabled = idle) {
                                    Text(stringResource(R.string.converter_record))
                                }
                            }
                            OutlinedButton(
                                onClick = { viewModel.playOriginal() },
                                enabled = state.hasRecording && idle,
                            ) {
                                Text(stringResource(R.string.converter_play_original))
                            }
                        }
                    }
                }

                // 3 — transpose (key)
                Card(Modifier.fillMaxWidth()) {
                    Column(
                        Modifier.padding(AvcrtSpacing.lg),
                        verticalArrangement = Arrangement.spacedBy(AvcrtSpacing.xs),
                    ) {
                        Text(
                            stringResource(R.string.converter_transpose, state.transpose),
                            style = MaterialTheme.typography.titleMedium,
                        )
                        Slider(
                            value = state.transpose.toFloat(),
                            onValueChange = { viewModel.setTranspose(it.roundToInt()) },
                            valueRange = -12f..12f,
                            steps = 23,
                            enabled = idle,
                        )
                        Text(
                            stringResource(R.string.converter_sample_rate),
                            style = MaterialTheme.typography.bodyMedium,
                        )
                        Row(horizontalArrangement = Arrangement.spacedBy(AvcrtSpacing.sm)) {
                            listOf(32000, 40000, 48000).forEach { sr ->
                                FilterChip(
                                    selected = state.modelSampleRate == sr,
                                    onClick = { viewModel.setSampleRate(sr) },
                                    label = { Text("${sr / 1000}k") },
                                    enabled = idle,
                                )
                            }
                        }
                    }
                }

                // 4 — convert
                Button(
                    onClick = { viewModel.convert() },
                    enabled = idle && state.sharedReady && state.selectedVoiceId != null && state.hasRecording,
                    modifier = Modifier.fillMaxWidth(),
                ) {
                    if (state.phase == ConverterPhase.CONVERTING) {
                        CircularProgressIndicator(Modifier.height(18.dp), strokeWidth = 2.dp)
                        Spacer(Modifier.width(AvcrtSpacing.sm))
                        Text(stringResource(R.string.converter_converting))
                    } else {
                        Text(stringResource(R.string.converter_convert))
                    }
                }

                // 5 — result
                if (state.hasResult) {
                    Row(
                        Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.spacedBy(AvcrtSpacing.sm),
                    ) {
                        Button(
                            onClick = { viewModel.playResult() },
                            enabled = idle,
                            modifier = Modifier.weight(1f),
                        ) {
                            Text(stringResource(R.string.converter_play_result))
                        }
                        OutlinedButton(
                            onClick = { viewModel.saveResult() },
                            enabled = idle,
                            modifier = Modifier.weight(1f),
                        ) {
                            Text(stringResource(R.string.converter_save))
                        }
                    }
                }

                // 6 — realtime live test (stutters on slow HW; smooth only on RTF<1 devices)
                Card(Modifier.fillMaxWidth()) {
                    Column(
                        Modifier.padding(AvcrtSpacing.lg),
                        verticalArrangement = Arrangement.spacedBy(AvcrtSpacing.sm),
                    ) {
                        Text(stringResource(R.string.converter_live_header), style = MaterialTheme.typography.titleMedium)
                        Text(
                            stringResource(R.string.converter_live_hint),
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                        if (state.phase == ConverterPhase.LIVE) {
                            Button(onClick = { viewModel.toggleLive() }) {
                                Text(stringResource(R.string.converter_live_stop))
                            }
                        } else {
                            Button(
                                onClick = { startLive() },
                                enabled = idle && state.sharedReady && state.selectedVoiceId != null,
                            ) {
                                Text(stringResource(R.string.converter_live))
                            }
                        }
                    }
                }
            }

            SnackbarHost(snackbar, modifier = Modifier.align(Alignment.BottomCenter))
        }
    }
}
