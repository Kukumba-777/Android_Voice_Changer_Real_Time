package dev.avcrt.data.preset

import androidx.datastore.core.DataStore
import androidx.datastore.preferences.core.MutablePreferences
import androidx.datastore.preferences.core.Preferences
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.stringPreferencesKey
import dev.avcrt.core.common.dispatcher.DispatcherProvider
import dev.avcrt.core.common.result.AvcrtError
import dev.avcrt.core.common.result.AvcrtResult
import dev.avcrt.core.datastore.SettingsDataStore
import dev.avcrt.domain.preset.Preset
import dev.avcrt.domain.preset.PresetOrigin
import dev.avcrt.domain.preset.PresetRepository
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.flow
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.withContext
import kotlinx.serialization.builtins.ListSerializer
import kotlinx.serialization.json.Json
import timber.log.Timber
import javax.inject.Inject
import javax.inject.Singleton

/**
 * Layered preset store:
 *   - read-only built-ins from [BuiltInPresetsLoader]
 *   - user customs serialized into the same Preferences DataStore as settings
 *
 * Built-ins are always first in the merged list and cannot be deleted; the
 * UI exposes "duplicate to custom" if a user wants to tweak one.
 */
@Singleton
class PresetRepositoryImpl @Inject constructor(
    private val builtIns: BuiltInPresetsLoader,
    @SettingsDataStore private val dataStore: DataStore<Preferences>,
    private val dispatchers: DispatcherProvider,
) : PresetRepository {

    private val json = Json {
        ignoreUnknownKeys = true
        encodeDefaults = true
    }

    // The encodeToString/decodeFromString reified-inline overloads can't infer
    // List<Preset> here, so we keep a concrete serializer around.
    private val presetListSerializer = ListSerializer(Preset.serializer())

    private val builtInsFlow: Flow<List<Preset>> = flow { emit(builtIns.load()) }

    override fun observeAll(): Flow<List<Preset>> = combine(
        builtInsFlow,
        customsFlow(),
    ) { builtInList, customList ->
        builtInList + customList
    }

    override fun observeActive(): Flow<String?> = dataStore.data.map { prefs ->
        prefs[ACTIVE_PRESET_KEY]
    }

    override suspend fun setActive(id: String?): AvcrtResult<Unit> = mutate { prefs ->
        if (id == null) prefs.remove(ACTIVE_PRESET_KEY) else prefs[ACTIVE_PRESET_KEY] = id
    }

    override suspend fun upsertCustom(preset: Preset): AvcrtResult<Unit> {
        if (preset.origin != PresetOrigin.CUSTOM) {
            return AvcrtResult.Failure(AvcrtError.Unknown("Only custom presets can be upserted"))
        }
        return mutate { prefs ->
            val existing = readCustoms(prefs).toMutableList()
            val idx = existing.indexOfFirst { it.id == preset.id }
            if (idx >= 0) existing[idx] = preset else existing.add(preset)
            prefs[CUSTOM_PRESETS_KEY] = json.encodeToString(presetListSerializer, existing)
        }
    }

    override suspend fun delete(id: String): AvcrtResult<Unit> {
        val isBuiltIn = withContext(dispatchers.io) {
            builtIns.load().any { it.id == id }
        }
        if (isBuiltIn) {
            return AvcrtResult.Failure(AvcrtError.Unknown("Built-in presets cannot be deleted"))
        }
        return mutate { prefs ->
            val remaining = readCustoms(prefs).filterNot { it.id == id }
            prefs[CUSTOM_PRESETS_KEY] = json.encodeToString(presetListSerializer, remaining)
        }
    }

    override suspend fun builtIns(): List<Preset> = builtIns.load()

    private fun customsFlow(): Flow<List<Preset>> = dataStore.data.map { readCustoms(it) }

    private fun readCustoms(prefs: Preferences): List<Preset> {
        val raw = prefs[CUSTOM_PRESETS_KEY] ?: return emptyList()
        return runCatching {
            json.decodeFromString(presetListSerializer, raw)
        }.onFailure { Timber.e(it, "Failed to decode custom presets") }
            .getOrDefault(emptyList())
    }

    private suspend inline fun mutate(crossinline block: (MutablePreferences) -> Unit): AvcrtResult<Unit> =
        runCatching {
            dataStore.edit { block(it) }
            AvcrtResult.Success(Unit)
        }.getOrElse { AvcrtResult.Failure(AvcrtError.Io("DataStore edit failed", it)) }

    private companion object {
        val ACTIVE_PRESET_KEY = stringPreferencesKey("active_preset_id")
        val CUSTOM_PRESETS_KEY = stringPreferencesKey("custom_presets_json")
    }
}
