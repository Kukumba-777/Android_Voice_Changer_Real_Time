package dev.avcrt.data.preset

import android.content.Context
import dagger.hilt.android.qualifiers.ApplicationContext
import dev.avcrt.core.common.dispatcher.DispatcherProvider
import dev.avcrt.domain.preset.Preset
import dev.avcrt.domain.preset.PresetDsp
import dev.avcrt.domain.preset.PresetEffects
import dev.avcrt.domain.preset.PresetOrigin
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json
import timber.log.Timber
import javax.inject.Inject
import javax.inject.Singleton

/**
 * Reads `assets/presets.json` once at first request and caches the parsed list.
 *
 * The schema is captured by the local [BuiltInPresetsManifest] DTO so the
 * file shape is decoupled from the domain model — we can change one without
 * forcing the other.
 */
@Singleton
class BuiltInPresetsLoader @Inject constructor(
    @ApplicationContext private val context: Context,
    private val dispatchers: DispatcherProvider,
) {
    private val json = Json {
        ignoreUnknownKeys = true
        isLenient = true
    }
    private val mutex = Mutex()
    @Volatile private var cached: List<Preset>? = null

    suspend fun load(): List<Preset> = mutex.withLock {
        cached?.let { return@withLock it }
        val parsed = withContext(dispatchers.io) {
            try {
                context.assets.open(ASSET_FILE).bufferedReader(Charsets.UTF_8).use { reader ->
                    val manifest = json.decodeFromString(BuiltInPresetsManifest.serializer(), reader.readText())
                    manifest.presets.map { it.toDomain() }
                }
            } catch (t: Throwable) {
                Timber.e(t, "Failed to load built-in presets")
                emptyList()
            }
        }
        cached = parsed
        parsed
    }

    @Serializable
    private data class BuiltInPresetsManifest(
        val version: Int,
        val presets: List<BuiltInPresetDto>,
    )

    @Serializable
    private data class BuiltInPresetDto(
        val id: String,
        val iconKey: String,
        val tags: List<String> = emptyList(),
        val names: Map<String, String>,
        val descriptions: Map<String, String>,
        val dsp: BuiltInDspDto,
    ) {
        fun toDomain(): Preset = Preset(
            id = id,
            origin = PresetOrigin.BUILT_IN,
            names = names,
            descriptions = descriptions,
            iconKey = iconKey,
            tags = tags,
            dsp = dsp.toDomain(),
        )
    }

    @Serializable
    private data class BuiltInDspDto(
        val pitchSemitones: Double,
        val formantSemitones: Double,
        val timeRatio: Double = 1.0,
        val gainDb: Double = 0.0,
        val crispness: Int = 5,
        val preserveFormants: Boolean = true,
        // Phase-6 effect chain. PresetEffects' field names == the JSON keys (dsp.fx.<effect>.<field>),
        // every field defaulted, so presets without `fx` (and the legacy ones) deserialize unchanged.
        val fx: PresetEffects = PresetEffects(),
        val bypassVoice: Boolean = false,
    ) {
        fun toDomain(): PresetDsp = PresetDsp(
            pitchSemitones = pitchSemitones,
            formantSemitones = formantSemitones,
            timeRatio = timeRatio,
            gainDb = gainDb,
            crispness = crispness,
            preserveFormants = preserveFormants,
            fx = fx,
            bypassVoice = bypassVoice,
        )
    }

    private companion object {
        const val ASSET_FILE = "presets.json"
    }
}
