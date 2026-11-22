package dev.avcrt.app.overlay

import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.rounded.Check
import androidx.compose.material.icons.rounded.Equalizer
import androidx.compose.material.icons.rounded.ExpandMore
import androidx.compose.material.icons.rounded.Mic
import androidx.compose.material.icons.rounded.MusicNote
import androidx.compose.material.icons.rounded.PlayArrow
import androidx.compose.material.icons.rounded.Stop
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.Dp
import dev.avcrt.app.R
import dev.avcrt.domain.media.MediaFile
import dev.avcrt.domain.media.SoundboardState
import dev.avcrt.domain.preset.Preset
import dev.avcrt.domain.preset.PresetOrigin
import java.text.Collator
import kotlin.math.roundToInt

private enum class OverlayTab { PRESETS, SOUNDS }
private enum class SoundSort { NAME, RECENT }
internal val BubbleSize = 54.dp

@Composable
internal fun OverlayRoot(
    expanded: Boolean,
    engineOn: Boolean,
    engineBusy: Boolean,
    presets: List<Preset>,
    activeId: String?,
    sounds: List<MediaFile>,
    playback: SoundboardState,
    appIcon: ImageBitmap,
    availableWidth: Dp,
    availableHeight: Dp,
    onDrag: (Float, Float) -> Unit,
    onToggleExpand: () -> Unit,
    onToggleEngine: (Boolean) -> Unit,
    onPickPreset: (String) -> Unit,
    onPlay: (String, Int) -> Unit,
    onStop: () -> Unit,
) {
    var tab by remember { mutableStateOf(OverlayTab.PRESETS) }
    var category by remember { mutableStateOf(PresetOrigin.BUILT_IN) }
    var sort by remember { mutableStateOf(SoundSort.NAME) }
    var clipVolume by remember { mutableFloatStateOf(100f) }
    val config = LocalConfiguration.current
    val locale = config.locales[0]
    val compact = availableHeight < 400.dp
    MaterialTheme(colorScheme = darkColorScheme()) {
        if (!expanded) {
            Surface(
                onClick = onToggleExpand,
                shape = CircleShape,
                color = Color.Transparent,
                modifier = Modifier.size(BubbleSize).draggable(onDrag),
            ) {
                Box(contentAlignment = Alignment.Center) {
                    Image(appIcon, stringResource(R.string.overlay_expand), Modifier.size(50.dp).clip(CircleShape))
                    Box(
                        Modifier.align(Alignment.BottomEnd).size(15.dp)
                            .background(Color.White, CircleShape).padding(2.dp)
                            .background(if (engineOn) Color(0xFF4CAF50) else Color(0xFF9E9E9E), CircleShape),
                    )
                }
            }
        } else {
            Surface(
                modifier = Modifier.width(minOf(availableWidth, if (compact) 330.dp else 300.dp))
                    .heightIn(max = availableHeight),
                shape = MaterialTheme.shapes.extraLarge,
                color = MaterialTheme.colorScheme.surfaceContainerHigh,
                shadowElevation = 8.dp,
            ) {
                Column(Modifier.padding(horizontal = 12.dp, vertical = 4.dp)) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Text(
                            stringResource(R.string.overlay_notification_title),
                            modifier = Modifier.weight(1f).draggable(onDrag).padding(vertical = if (compact) 2.dp else 10.dp),
                            style = MaterialTheme.typography.titleMedium,
                        )
                        if (compact) {
                            val engineLabel = stringResource(R.string.overlay_engine)
                            Switch(
                                checked = engineOn,
                                enabled = !engineBusy,
                                onCheckedChange = onToggleEngine,
                                modifier = Modifier.semantics { contentDescription = engineLabel },
                            )
                        }
                        IconButton(onClick = onToggleExpand, modifier = Modifier.size(44.dp)) {
                            Icon(Icons.Rounded.ExpandMore, stringResource(R.string.overlay_collapse))
                        }
                    }
                    HorizontalDivider()
                    if (!compact) Row(verticalAlignment = Alignment.CenterVertically) {
                        Icon(Icons.Rounded.Mic, null, modifier = Modifier.size(20.dp))
                        Text(
                            stringResource(R.string.overlay_engine),
                            modifier = Modifier.weight(1f).padding(horizontal = 8.dp),
                            style = MaterialTheme.typography.bodyMedium,
                        )
                        Switch(checked = engineOn, enabled = !engineBusy, onCheckedChange = onToggleEngine)
                    }
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        OverlayTab.entries.forEach { entry ->
                            val presetsTab = entry == OverlayTab.PRESETS
                            FilterChip(
                                selected = tab == entry,
                                onClick = { tab = entry },
                                label = { Text(stringResource(if (presetsTab) R.string.overlay_presets else R.string.overlay_sounds)) },
                                leadingIcon = {
                                    Icon(if (presetsTab) Icons.Rounded.Equalizer else Icons.Rounded.MusicNote, null, Modifier.size(18.dp))
                                },
                                modifier = Modifier.weight(1f),
                            )
                        }
                    }
                    when (tab) {
                        OverlayTab.PRESETS -> {
                            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                                PresetOrigin.entries.forEach { origin ->
                                    FilterChip(
                                        selected = category == origin,
                                        onClick = { category = origin },
                                        label = { Text(stringResource(if (origin == PresetOrigin.BUILT_IN) R.string.overlay_builtin else R.string.overlay_custom)) },
                                    )
                                }
                            }
                            val shown = remember(presets, category) { presets.filter { it.origin == category } }
                            if (shown.isEmpty()) OverlayHint(R.string.overlay_no_presets)
                            else LazyColumn(Modifier.heightIn(max = 220.dp)) {
                                items(shown, key = { it.id }) { preset ->
                                    val selected = preset.id == activeId
                                    Row(
                                        modifier = Modifier.fillMaxWidth()
                                            .selectable(selected, enabled = !engineBusy, role = Role.RadioButton) { onPickPreset(preset.id) }
                                            .heightIn(min = 42.dp).padding(horizontal = 6.dp),
                                        verticalAlignment = Alignment.CenterVertically,
                                    ) {
                                        Text(preset.displayName(locale.language), Modifier.weight(1f), maxLines = 1, overflow = TextOverflow.Ellipsis)
                                        if (selected) Icon(Icons.Rounded.Check, null, tint = MaterialTheme.colorScheme.primary)
                                    }
                                }
                            }
                        }
                        OverlayTab.SOUNDS -> {
                            if (!engineOn && !compact) OverlayHint(R.string.overlay_sound_enable)
                            if (sounds.isEmpty()) OverlayHint(R.string.overlay_no_sounds)
                            else {
                                val sorted = remember(sounds, sort, locale) {
                                    val collator = Collator.getInstance(locale)
                                    val byName = Comparator<MediaFile> { a, b -> collator.compare(a.id, b.id) }
                                    when (sort) {
                                        SoundSort.NAME -> sounds.sortedWith(byName)
                                        SoundSort.RECENT -> sounds.sortedWith(compareByDescending<MediaFile> { it.modifiedAtMillis }.then(byName))
                                    }
                                }
                                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                                    SoundSort.entries.forEach { entry ->
                                        FilterChip(
                                            selected = sort == entry,
                                            onClick = { sort = entry },
                                            label = { Text(stringResource(if (entry == SoundSort.NAME) R.string.overlay_sort_name else R.string.overlay_sort_recent)) },
                                        )
                                    }
                                }
                                val volumeLabel = stringResource(R.string.overlay_clip_volume, clipVolume.roundToInt())
                                Row(verticalAlignment = Alignment.CenterVertically) {
                                    Text(volumeLabel, style = MaterialTheme.typography.labelMedium)
                                    Slider(
                                        value = clipVolume,
                                        onValueChange = { clipVolume = it },
                                        valueRange = 10f..100f,
                                        steps = 8,
                                        enabled = playback == SoundboardState.Idle,
                                        modifier = Modifier.weight(1f).padding(start = 8.dp)
                                            .semantics { contentDescription = volumeLabel },
                                    )
                                }
                                LazyColumn(Modifier.heightIn(max = 220.dp)) {
                                    items(sorted, key = { it.id }) { sound ->
                                        SoundRow(sound, engineOn && !engineBusy, playback,
                                            { id -> onPlay(id, clipVolume.roundToInt()) }, onStop)
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun SoundRow(
    sound: MediaFile,
    enabled: Boolean,
    state: SoundboardState,
    onPlay: (String) -> Unit,
    onStop: () -> Unit,
) {
    val preparing = state is SoundboardState.Preparing && state.id == sound.id
    val active = preparing || (state is SoundboardState.Playing && state.id == sound.id)
    Row(Modifier.fillMaxWidth().padding(start = 6.dp), verticalAlignment = Alignment.CenterVertically) {
        Text(sound.displayName, Modifier.weight(1f), maxLines = 1, overflow = TextOverflow.Ellipsis)
        IconButton(
            onClick = { if (active) onStop() else onPlay(sound.id) },
            enabled = active || enabled,
        ) {
            Box(contentAlignment = Alignment.Center) {
                if (preparing) CircularProgressIndicator(Modifier.size(28.dp), strokeWidth = 2.dp)
                Icon(
                    if (active) Icons.Rounded.Stop else Icons.Rounded.PlayArrow,
                    stringResource(if (active) R.string.overlay_stop_clip else R.string.overlay_play_clip, sound.displayName),
                    Modifier.size(if (preparing) 18.dp else 24.dp),
                )
            }
        }
    }
}

@Composable
private fun OverlayHint(res: Int) {
    Text(stringResource(res), Modifier.padding(vertical = 8.dp), style = MaterialTheme.typography.bodySmall,
        color = MaterialTheme.colorScheme.onSurfaceVariant)
}

@Composable
private fun Modifier.draggable(onDrag: (Float, Float) -> Unit): Modifier {
    val callback by rememberUpdatedState(onDrag)
    return pointerInput(Unit) {
        detectDragGestures { change, drag ->
            change.consume()
            callback(drag.x, drag.y)
        }
    }
}
