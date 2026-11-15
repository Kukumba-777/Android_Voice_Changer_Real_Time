package dev.avcrt.domain.preset

import kotlinx.serialization.Serializable

/**
 * A voice preset. Comes from one of two places:
 *   - bundled `presets.json` in the app's assets (origin = [PresetOrigin.BUILT_IN])
 *   - user-created and persisted by [PresetRepository]    (origin = [PresetOrigin.CUSTOM])
 *
 * The name and description are kept as localized maps so the data layer doesn't
 * have to know what the user's current locale is — UI picks the right string.
 */
@Serializable
data class Preset(
    val id: String,
    val origin: PresetOrigin,
    val names: Map<String, String>,
    val descriptions: Map<String, String>,
    val iconKey: String,
    val dsp: PresetDsp,
    val tags: List<String> = emptyList(),
) {
    fun displayName(localeTag: String, fallbackLocale: String = "en"): String =
        names[localeTag] ?: names[fallbackLocale] ?: names.values.firstOrNull() ?: id

    fun displayDescription(localeTag: String, fallbackLocale: String = "en"): String? =
        descriptions[localeTag] ?: descriptions[fallbackLocale] ?: descriptions.values.firstOrNull()
}

enum class PresetOrigin { BUILT_IN, CUSTOM }
