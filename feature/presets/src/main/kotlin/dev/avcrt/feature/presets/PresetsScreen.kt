package dev.avcrt.feature.presets

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.Add
import androidx.compose.material.icons.rounded.ContentCopy
import androidx.compose.material.icons.rounded.DeleteOutline
import androidx.compose.material.icons.rounded.Edit
import androidx.compose.material.icons.rounded.MoreVert
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExtendedFloatingActionButton
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Tab
import androidx.compose.material3.TabRow
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.navigation.NavController
import dev.avcrt.core.ui.AvcrtSpacing
import dev.avcrt.core.ui.components.AvcrtScreen
import dev.avcrt.domain.preset.Preset
import dev.avcrt.domain.preset.PresetOrigin

@Composable
fun PresetsScreen(
    navController: NavController,
    viewModel: PresetsViewModel = hiltViewModel(),
) {
    val state by viewModel.state.collectAsStateWithLifecycle()
    val editTarget by viewModel.editTarget.collectAsStateWithLifecycle()
    val snackbar = remember { SnackbarHostState() }
    var pendingDelete by remember { mutableStateOf<Preset?>(null) }

    LaunchedEffect(state.transientMessage) {
        state.transientMessage?.let {
            snackbar.showSnackbar(it)
            viewModel.consumeTransient()
        }
    }
    LaunchedEffect(editTarget) {
        editTarget?.let { id ->
            viewModel.consumeEditTarget()
            navController.navigate(PresetEditorRoute.edit(id))
        }
    }

    AvcrtScreen(
        title = stringResource(R.string.presets_title),
        floatingActionButton = {
            ExtendedFloatingActionButton(
                onClick = { navController.navigate(PresetEditorRoute.create()) },
                icon = { Icon(Icons.Rounded.Add, contentDescription = null) },
                text = { Text(stringResource(R.string.presets_create_custom)) },
            )
        },
    ) { _ ->
        Box(modifier = Modifier.fillMaxSize()) {
            Column(modifier = Modifier.fillMaxSize()) {
                // Two tabs split the list — Custom presets vs Built-in — replacing the old single
                // scroll that stacked both section headers. Selection survives config changes.
                var selectedTab by rememberSaveable { mutableStateOf(0) }
                TabRow(selectedTabIndex = selectedTab) {
                    Tab(
                        selected = selectedTab == 0,
                        onClick = { selectedTab = 0 },
                        text = { Text(stringResource(R.string.presets_custom_section)) },
                    )
                    Tab(
                        selected = selectedTab == 1,
                        onClick = { selectedTab = 1 },
                        text = { Text(stringResource(R.string.presets_builtin_section)) },
                    )
                }
                val shown = if (selectedTab == 0) state.customs else state.builtIns
                LazyColumn(
                    modifier = Modifier
                        .fillMaxSize()
                        .padding(horizontal = AvcrtSpacing.lg),
                    contentPadding = PaddingValues(top = AvcrtSpacing.md, bottom = 96.dp),
                    verticalArrangement = Arrangement.spacedBy(AvcrtSpacing.sm),
                ) {
                    if (selectedTab == 0 && shown.isEmpty()) {
                        item {
                            Text(
                                text = stringResource(R.string.presets_custom_empty),
                                style = MaterialTheme.typography.bodyMedium,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                                modifier = Modifier.padding(vertical = AvcrtSpacing.md),
                            )
                        }
                    } else {
                        items(shown, key = { it.id }) { preset ->
                            PresetCard(
                                preset = preset,
                                isActive = preset.id == state.activeId,
                                isApplying = state.applyingId == preset.id,
                                onApply = { viewModel.onApply(preset) },
                                onEdit = { navController.navigate(PresetEditorRoute.edit(preset.id)) },
                                onDuplicate = { viewModel.onDuplicate(preset) },
                                onDelete = { pendingDelete = preset },
                            )
                        }
                    }
                }
            }
            SnackbarHost(hostState = snackbar, modifier = Modifier.align(Alignment.BottomCenter))
        }
    }

    pendingDelete?.let { preset ->
        AlertDialog(
            onDismissRequest = { pendingDelete = null },
            title = { Text(stringResource(R.string.presets_editor_delete_confirm_title)) },
            text = { Text(stringResource(R.string.presets_editor_delete_confirm_text)) },
            confirmButton = {
                TextButton(onClick = {
                    viewModel.onDelete(preset.id)
                    pendingDelete = null
                }) { Text(stringResource(R.string.presets_delete)) }
            },
            dismissButton = {
                TextButton(onClick = { pendingDelete = null }) {
                    Text(stringResource(R.string.presets_cancel))
                }
            },
        )
    }
}

@Composable
private fun PresetCard(
    preset: Preset,
    isActive: Boolean,
    isApplying: Boolean,
    onApply: () -> Unit,
    onEdit: () -> Unit,
    onDuplicate: () -> Unit,
    onDelete: () -> Unit,
) {
    val localeTag = currentLocaleTag()
    Card(
        modifier = Modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(
            containerColor = if (isActive) MaterialTheme.colorScheme.primaryContainer
            else MaterialTheme.colorScheme.surfaceVariant,
        ),
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(
                    start = AvcrtSpacing.lg,
                    top = AvcrtSpacing.lg,
                    end = AvcrtSpacing.sm,
                    bottom = AvcrtSpacing.lg,
                ),
            verticalArrangement = Arrangement.spacedBy(AvcrtSpacing.sm),
        ) {
            Text(
                text = preset.displayName(localeTag),
                style = MaterialTheme.typography.titleMedium,
            )
            preset.displayDescription(localeTag)?.let { description ->
                Text(
                    text = description,
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            Spacer(Modifier.height(AvcrtSpacing.xs))
            Row(
                horizontalArrangement = Arrangement.spacedBy(AvcrtSpacing.md),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                if (!preset.dsp.bypassVoice) {
                    PresetChip(
                        label = stringResource(R.string.presets_pitch),
                        value = formatSemitones(preset.dsp.pitchSemitones),
                    )
                    PresetChip(
                        label = stringResource(R.string.presets_formant),
                        value = formatSemitones(preset.dsp.formantSemitones),
                    )
                }
                Spacer(Modifier.weight(1f))
                // Fixed-height action slot so the card never reflows as it swaps
                // Apply ↔ Active ↔ spinner.
                Box(
                    modifier = Modifier.heightIn(min = 40.dp),
                    contentAlignment = Alignment.Center,
                ) {
                    when {
                        isApplying -> CircularProgressIndicator(modifier = Modifier.height(24.dp))
                        isActive -> Text(
                            text = stringResource(R.string.presets_applied),
                            style = MaterialTheme.typography.labelLarge,
                            color = MaterialTheme.colorScheme.primary,
                        )
                        else -> Button(onClick = onApply) {
                            Text(stringResource(R.string.presets_apply))
                        }
                    }
                }
                PresetCardMenu(
                    isCustom = preset.origin == PresetOrigin.CUSTOM,
                    onEdit = onEdit,
                    onDuplicate = onDuplicate,
                    onDelete = onDelete,
                )
            }
        }
    }
}

@Composable
private fun PresetCardMenu(
    isCustom: Boolean,
    onEdit: () -> Unit,
    onDuplicate: () -> Unit,
    onDelete: () -> Unit,
) {
    var expanded by remember { mutableStateOf(false) }
    Box {
        IconButton(onClick = { expanded = true }) {
            Icon(
                Icons.Rounded.MoreVert,
                contentDescription = stringResource(R.string.presets_menu),
            )
        }
        DropdownMenu(expanded = expanded, onDismissRequest = { expanded = false }) {
            if (isCustom) {
                DropdownMenuItem(
                    text = { Text(stringResource(R.string.presets_edit)) },
                    leadingIcon = { Icon(Icons.Rounded.Edit, contentDescription = null) },
                    onClick = {
                        expanded = false
                        onEdit()
                    },
                )
                DropdownMenuItem(
                    text = { Text(stringResource(R.string.presets_delete)) },
                    leadingIcon = { Icon(Icons.Rounded.DeleteOutline, contentDescription = null) },
                    onClick = {
                        expanded = false
                        onDelete()
                    },
                )
            } else {
                DropdownMenuItem(
                    text = { Text(stringResource(R.string.presets_duplicate)) },
                    leadingIcon = { Icon(Icons.Rounded.ContentCopy, contentDescription = null) },
                    onClick = {
                        expanded = false
                        onDuplicate()
                    },
                )
            }
        }
    }
}

@Composable
private fun PresetChip(label: String, value: String) {
    Column {
        Text(
            text = label,
            style = MaterialTheme.typography.labelSmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Text(
            text = value,
            style = MaterialTheme.typography.labelLarge,
        )
    }
}
