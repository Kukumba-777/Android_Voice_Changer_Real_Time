package dev.avcrt.app.overlay

import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.darkColorScheme
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Check
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.graphics.drawable.toBitmap
import dev.avcrt.domain.preset.Preset
import dev.avcrt.domain.preset.PresetOrigin

/**
 * The whole floating overlay UI: a draggable collapsed bubble that expands into a compact
 * vertical control card. Stateless — the [OverlayService] owns the state and supplies callbacks.
 * A tap expands/collapses; a drag moves the window (see [draggableTap]).
 */
@Composable
fun OverlayRoot(
    expanded: Boolean,
    engineOn: Boolean,
    category: PresetOrigin,
    presets: List<Preset>,
    activeId: String?,
    localeTag: String,
    onDrag: (Float, Float) -> Unit,
    onToggleExpand: () -> Unit,
    onClose: () -> Unit,
    onToggleEngine: (Boolean) -> Unit,
    onPickCategory: (PresetOrigin) -> Unit,
    onPickPreset: (String) -> Unit,
    onPlaySoundClick: () -> Unit,
) {
    // Use a fixed dark scheme so the overlay reads cleanly over any app/background.
    MaterialTheme(colorScheme = darkColorScheme()) {
        if (!expanded) {
            CollapsedBubble(engineOn, onDrag, onToggleExpand)
        } else {
            ExpandedCard(
                engineOn = engineOn,
                category = category,
                presets = presets,
                activeId = activeId,
                localeTag = localeTag,
                onDrag = onDrag,
                onClose = onClose,
                onToggleEngine = onToggleEngine,
                onPickCategory = onPickCategory,
                onPickPreset = onPickPreset,
                onPlaySoundClick = onPlaySoundClick,
            )
        }
    }
}

/**
 * Combines a TAP (expand/collapse) with a DRAG (move the window). Two separate pointerInput handlers:
 * detectDragGestures alone never fires for a pure tap (it needs to cross the touch slop first), which
 * is why the bubble didn't respond to a tap — detectTapGestures handles that case.
 */
private fun Modifier.draggableTap(
    onDrag: (Float, Float) -> Unit,
    onTap: () -> Unit,
): Modifier = this
    .pointerInput(Unit) { detectTapGestures(onTap = { onTap() }) }
    .pointerInput(Unit) {
        detectDragGestures { change, drag ->
            change.consume()
            onDrag(drag.x, drag.y)
        }
    }

@Composable
private fun CollapsedBubble(
    engineOn: Boolean,
    onDrag: (Float, Float) -> Unit,
    onTap: () -> Unit,
) {
    val context = LocalContext.current
    // The real app icon (adaptive icons render fine once rasterized to a bitmap).
    val appIcon = remember {
        context.packageManager.getApplicationIcon(context.applicationInfo).toBitmap().asImageBitmap()
    }
    Box(
        modifier = Modifier
            .size(54.dp)
            .draggableTap(onDrag, onTap),
        contentAlignment = Alignment.Center,
    ) {
        Image(
            bitmap = appIcon,
            contentDescription = "AVCRT",
            modifier = Modifier
                .size(50.dp)
                .clip(CircleShape),
        )
        // on/off status dot with a white ring so it reads over the icon.
        Box(
            modifier = Modifier
                .align(Alignment.BottomEnd)
                .size(15.dp)
                .clip(CircleShape)
                .background(Color.White)
                .padding(2.dp)
                .clip(CircleShape)
                .background(if (engineOn) Color(0xFF4CAF50) else Color(0xFF9E9E9E)),
        )
    }
}

@Composable
private fun ExpandedCard(
    engineOn: Boolean,
    category: PresetOrigin,
    presets: List<Preset>,
    activeId: String?,
    localeTag: String,
    onDrag: (Float, Float) -> Unit,
    onClose: () -> Unit,
    onToggleEngine: (Boolean) -> Unit,
    onPickCategory: (PresetOrigin) -> Unit,
    onPickPreset: (String) -> Unit,
    onPlaySoundClick: () -> Unit,
) {
    Surface(
        shape = RoundedCornerShape(20.dp),
        color = MaterialTheme.colorScheme.surface,
        shadowElevation = 10.dp,
        modifier = Modifier.widthIn(min = 230.dp, max = 270.dp),
    ) {
        Column(Modifier.padding(horizontal = 14.dp, vertical = 10.dp)) {
            // Header (drag handle) + collapse
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(
                    "AVCRT",
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.Bold,
                    modifier = Modifier
                        .weight(1f)
                        .draggableTap(onDrag, onClose),
                )
                IconButton(onClick = onClose, modifier = Modifier.size(28.dp)) {
                    Icon(Icons.Filled.Close, contentDescription = "Свернуть")
                }
            }

            ThinDivider()

            // 1) Voice on/off
            Row(
                verticalAlignment = Alignment.CenterVertically,
                modifier = Modifier.fillMaxWidth().padding(vertical = 2.dp),
            ) {
                Text("🎤", fontSize = 16.sp)
                Spacer(Modifier.width(8.dp))
                Text(
                    "Изменение голоса",
                    style = MaterialTheme.typography.bodyMedium,
                    modifier = Modifier.weight(1f),
                )
                Switch(checked = engineOn, onCheckedChange = onToggleEngine)
            }

            ThinDivider()

            // 2) Category (built-in / custom)
            Row(
                verticalAlignment = Alignment.CenterVertically,
                modifier = Modifier.fillMaxWidth().padding(vertical = 4.dp),
            ) {
                Text("📂", fontSize = 16.sp)
                Spacer(Modifier.width(8.dp))
                CategoryPill(
                    label = "Встроенные",
                    selected = category == PresetOrigin.BUILT_IN,
                    onClick = { onPickCategory(PresetOrigin.BUILT_IN) },
                )
                Spacer(Modifier.width(6.dp))
                CategoryPill(
                    label = "Свои",
                    selected = category == PresetOrigin.CUSTOM,
                    onClick = { onPickCategory(PresetOrigin.CUSTOM) },
                )
            }

            // 3) Preset list (scrolls if long)
            Column(
                Modifier
                    .fillMaxWidth()
                    .heightIn(max = 200.dp)
                    .verticalScroll(rememberScrollState()),
            ) {
                if (presets.isEmpty()) {
                    Text(
                        "Нет пресетов",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.padding(vertical = 8.dp),
                    )
                } else {
                    presets.forEach { p ->
                        PresetRow(
                            name = p.displayName(localeTag),
                            selected = p.id == activeId,
                            onClick = { onPickPreset(p.id) },
                        )
                    }
                }
            }

            ThinDivider()

            // 4) Soundboard (Stage 3 fills the sound list; the row is wired to open it)
            Row(
                verticalAlignment = Alignment.CenterVertically,
                modifier = Modifier
                    .fillMaxWidth()
                    .clickable(onClick = onPlaySoundClick)
                    .padding(vertical = 8.dp),
            ) {
                Text("🎵", fontSize = 16.sp)
                Spacer(Modifier.width(8.dp))
                Text(
                    "Играть звук в звонок",
                    style = MaterialTheme.typography.bodyMedium,
                    modifier = Modifier.weight(1f),
                )
                Icon(Icons.Filled.PlayArrow, contentDescription = null, modifier = Modifier.size(20.dp))
            }
        }
    }
}

@Composable
private fun ThinDivider() {
    Box(
        Modifier
            .fillMaxWidth()
            .padding(vertical = 4.dp)
            .background(MaterialTheme.colorScheme.outlineVariant)
            .heightIn(min = 1.dp, max = 1.dp),
    )
}

@Composable
private fun CategoryPill(label: String, selected: Boolean, onClick: () -> Unit) {
    Surface(
        shape = RoundedCornerShape(50),
        color = if (selected) MaterialTheme.colorScheme.primary
        else MaterialTheme.colorScheme.surfaceVariant,
        modifier = Modifier.clickable(onClick = onClick),
    ) {
        Text(
            label,
            style = MaterialTheme.typography.labelMedium,
            color = if (selected) MaterialTheme.colorScheme.onPrimary
            else MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.padding(horizontal = 12.dp, vertical = 5.dp),
        )
    }
}

@Composable
private fun PresetRow(name: String, selected: Boolean, onClick: () -> Unit) {
    Row(
        verticalAlignment = Alignment.CenterVertically,
        modifier = Modifier
            .fillMaxWidth()
            .clickable(onClick = onClick)
            .padding(vertical = 7.dp, horizontal = 2.dp),
    ) {
        Text(
            name,
            style = MaterialTheme.typography.bodyMedium,
            fontWeight = if (selected) FontWeight.Bold else FontWeight.Normal,
            color = if (selected) MaterialTheme.colorScheme.primary
            else MaterialTheme.colorScheme.onSurface,
            modifier = Modifier.weight(1f),
        )
        if (selected) {
            Icon(
                Icons.Filled.Check,
                contentDescription = null,
                tint = MaterialTheme.colorScheme.primary,
                modifier = Modifier.size(18.dp),
            )
        }
    }
}
