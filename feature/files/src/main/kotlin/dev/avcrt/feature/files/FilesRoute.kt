package dev.avcrt.feature.files

import android.text.format.Formatter
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.AudioFile
import androidx.compose.material.icons.outlined.DeleteOutline
import androidx.compose.material.icons.outlined.Description
import androidx.compose.material.icons.outlined.FileUpload
import androidx.compose.material.icons.outlined.Folder
import androidx.compose.material.icons.outlined.MoreVert
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.ListItem
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
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextOverflow
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.navigation.NavGraphBuilder
import androidx.navigation.compose.composable
import dev.avcrt.core.ui.AvcrtSpacing
import dev.avcrt.core.ui.components.AvcrtScreen
import dev.avcrt.domain.media.MediaFolder
import java.text.Collator

object FilesRoute { const val path = "files" }

fun NavGraphBuilder.filesGraph() {
    composable(FilesRoute.path) { FilesScreen() }
}

@Composable
private fun FilesScreen(vm: FilesViewModel = hiltViewModel()) {
    var folder by rememberSaveable { mutableStateOf(MediaFolder.MODELS) }
    var pendingDelete by rememberSaveable { mutableStateOf<String?>(null) }
    var showBackupMenu by remember { mutableStateOf(false) }
    var confirmRestore by rememberSaveable { mutableStateOf(false) }
    val files by vm.files.getValue(folder).collectAsStateWithLifecycle()
    val busy by vm.busy.collectAsStateWithLifecycle()
    val message by vm.message.collectAsStateWithLifecycle()
    val modelsPicker = rememberLauncherForActivityResult(ActivityResultContracts.OpenMultipleDocuments()) {
        vm.import(MediaFolder.MODELS, it)
    }
    val soundsPicker = rememberLauncherForActivityResult(ActivityResultContracts.OpenMultipleDocuments()) {
        vm.import(MediaFolder.SOUNDS, it)
    }
    val backupExport = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/zip")) {
        it?.let(vm::exportBackup)
    }
    val backupRestore = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) {
        it?.let(vm::restoreBackup)
    }
    val context = LocalContext.current
    val locale = context.resources.configuration.locales[0]
    val sorted = remember(files, locale) {
        val collator = Collator.getInstance(locale)
        files.sortedWith { a, b -> collator.compare(a.id, b.id) }
    }
    val snackbar = remember { SnackbarHostState() }
    LaunchedEffect(message) {
        message?.let { snackbar.showSnackbar(it); vm.consumeMessage() }
    }

    AvcrtScreen(
        title = stringResource(R.string.files_title),
        snackbarHost = { SnackbarHost(snackbar) },
        actions = {
            IconButton(onClick = { showBackupMenu = true }, enabled = !busy) {
                Icon(Icons.Outlined.MoreVert, stringResource(R.string.files_backup))
            }
            DropdownMenu(expanded = showBackupMenu, onDismissRequest = { showBackupMenu = false }) {
                DropdownMenuItem(text = { Text(stringResource(R.string.files_backup_export)) }, onClick = {
                    showBackupMenu = false
                    backupExport.launch("AVCRT-backup.zip")
                })
                DropdownMenuItem(text = { Text(stringResource(R.string.files_backup_restore)) }, onClick = {
                    showBackupMenu = false
                    confirmRestore = true
                })
            }
        },
    ) { padding ->
        Column(Modifier.fillMaxSize().padding(padding)) {
            TabRow(selectedTabIndex = folder.ordinal) {
                MediaFolder.entries.forEach { entry ->
                    Tab(
                        selected = folder == entry,
                        onClick = { folder = entry; pendingDelete = null },
                        text = { Text(stringResource(entry.titleRes)) },
                        icon = { Icon(Icons.Outlined.Folder, contentDescription = null) },
                    )
                }
            }
            Text(
                if (folder == MediaFolder.MODELS) stringResource(R.string.files_models_hint)
                else stringResource(R.string.files_sounds_hint),
                modifier = Modifier.padding(AvcrtSpacing.lg),
                style = MaterialTheme.typography.bodyMedium,
            )
            FilledTonalButton(
                onClick = {
                    // Providers sometimes mislabel audio and ONNX; validate the selected files instead.
                    if (folder == MediaFolder.MODELS) modelsPicker.launch(arrayOf("*/*"))
                    else soundsPicker.launch(arrayOf("*/*"))
                },
                enabled = !busy,
                modifier = Modifier.padding(horizontal = AvcrtSpacing.lg),
            ) {
                Icon(Icons.Outlined.FileUpload, contentDescription = null)
                Text(stringResource(R.string.files_import), Modifier.padding(start = AvcrtSpacing.sm))
            }
            if (busy) LinearProgressIndicator(Modifier.fillMaxWidth())
            LazyColumn(Modifier.weight(1f)) {
                if (sorted.isEmpty() && !busy) item {
                    Text(stringResource(R.string.files_empty), Modifier.padding(AvcrtSpacing.lg))
                }
                items(sorted, key = { it.id }) { file ->
                    ListItem(
                        headlineContent = { Text(file.id, maxLines = 2, overflow = TextOverflow.Ellipsis) },
                        supportingContent = { Text(Formatter.formatFileSize(context, file.sizeBytes)) },
                        leadingContent = {
                            Icon(if (folder == MediaFolder.MODELS) Icons.Outlined.Description else Icons.Outlined.AudioFile, null)
                        },
                        trailingContent = {
                            IconButton(onClick = { pendingDelete = file.id }, enabled = !busy) {
                                Icon(Icons.Outlined.DeleteOutline, stringResource(R.string.files_delete))
                            }
                        },
                    )
                }
            }
        }
    }
    if (confirmRestore) AlertDialog(
        onDismissRequest = { confirmRestore = false },
        title = { Text(stringResource(R.string.files_backup_restore)) },
        text = { Text(stringResource(R.string.files_backup_restore_hint)) },
        confirmButton = {
            TextButton(onClick = {
                confirmRestore = false
                backupRestore.launch(arrayOf("application/zip", "application/octet-stream", "application/x-zip-compressed"))
            }) { Text(stringResource(R.string.files_backup_choose)) }
        },
        dismissButton = {
            TextButton(onClick = { confirmRestore = false }) { Text(stringResource(android.R.string.cancel)) }
        },
    )
    pendingDelete?.let { name ->
        AlertDialog(
            onDismissRequest = { pendingDelete = null },
            title = { Text(stringResource(R.string.files_delete_title)) },
            text = { Text(name) },
            confirmButton = {
                TextButton(onClick = { pendingDelete = null; vm.delete(folder, name) }) {
                    Text(stringResource(R.string.files_delete))
                }
            },
            dismissButton = {
                TextButton(onClick = { pendingDelete = null }) { Text(stringResource(android.R.string.cancel)) }
            },
        )
    }
}

private val MediaFolder.titleRes get() = when (this) {
    MediaFolder.MODELS -> R.string.files_models
    MediaFolder.SOUNDS -> R.string.files_sounds
}
