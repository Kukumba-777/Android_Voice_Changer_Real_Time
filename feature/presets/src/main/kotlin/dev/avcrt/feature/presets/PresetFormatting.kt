package dev.avcrt.feature.presets

import androidx.compose.runtime.Composable
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource

/** BCP-47 language tag of the device's current locale, used to read/write the
 *  locale-keyed name/description maps on [dev.avcrt.domain.preset.Preset]. */
@Composable
internal fun currentLocaleTag(): String {
    val context = LocalContext.current
    val locales = context.resources.configuration.locales
    return if (locales.isEmpty) "en" else locales[0].language.ifBlank { "en" }
}

@Composable
internal fun formatSemitones(value: Double): String {
    val unit = stringResource(R.string.presets_semitones_short)
    val sign = if (value > 0) "+" else ""
    return "$sign${"%.1f".format(value)} $unit"
}

@Composable
internal fun formatDb(value: Double): String {
    val unit = stringResource(R.string.presets_editor_db_short)
    val sign = if (value > 0) "+" else ""
    return "$sign${"%.1f".format(value)} $unit"
}
