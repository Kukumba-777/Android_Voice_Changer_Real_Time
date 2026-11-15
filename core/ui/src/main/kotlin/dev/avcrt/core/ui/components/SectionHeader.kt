package dev.avcrt.core.ui.components

import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import dev.avcrt.core.ui.AvcrtSpacing

@Composable
fun SectionHeader(
    title: String,
    modifier: Modifier = Modifier,
    padding: PaddingValues = PaddingValues(
        start = AvcrtSpacing.lg,
        end = AvcrtSpacing.lg,
        top = AvcrtSpacing.lg,
        bottom = AvcrtSpacing.sm,
    ),
) {
    Text(
        text = title,
        style = MaterialTheme.typography.titleSmall,
        color = MaterialTheme.colorScheme.primary,
        modifier = modifier.padding(padding),
    )
}
