package dev.avcrt.data.settings

import androidx.datastore.core.DataStore
import androidx.datastore.preferences.core.MutablePreferences
import androidx.datastore.preferences.core.Preferences
import androidx.datastore.preferences.core.booleanPreferencesKey
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.stringPreferencesKey
import dev.avcrt.core.common.dispatcher.DispatcherProvider
import dev.avcrt.core.common.result.AvcrtError
import dev.avcrt.core.common.result.AvcrtResult
import dev.avcrt.core.datastore.SettingsDataStore
import dev.avcrt.domain.settings.AppLocale
import dev.avcrt.domain.settings.AppSettings
import dev.avcrt.domain.settings.SettingsRepository
import dev.avcrt.domain.settings.ThemeMode
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.runBlocking
import javax.inject.Inject
import javax.inject.Singleton

/**
 * DataStore-backed settings store.
 *
 * The mapping between domain enums and on-disk strings is the only place that
 * knows the wire format. Adding a new field = add a key here + extend
 * [AppSettings].
 */
@Singleton
class SettingsRepositoryImpl @Inject constructor(
    @SettingsDataStore private val dataStore: DataStore<Preferences>,
    private val dispatchers: DispatcherProvider,
) : SettingsRepository {

    private val settingsScope = CoroutineScope(SupervisorJob() + dispatchers.io)

    private val settingsFlow: StateFlow<AppSettings> = dataStore.data
        .map { prefs -> prefs.toAppSettings() }
        .stateIn(
            scope = settingsScope,
            started = SharingStarted.Eagerly,
            initialValue = AppSettings(),
        )

    override fun observe(): Flow<AppSettings> = settingsFlow

    override fun snapshot(): AppSettings = settingsFlow.value

    override suspend fun setThemeMode(mode: ThemeMode): AvcrtResult<Unit> = mutate { prefs ->
        prefs[KEY_THEME_MODE] = mode.name
    }

    override suspend fun setUseDynamicColor(enabled: Boolean): AvcrtResult<Unit> = mutate { prefs ->
        prefs[KEY_USE_DYNAMIC_COLOR] = enabled
    }

    override suspend fun setLocale(locale: AppLocale): AvcrtResult<Unit> = mutate { prefs ->
        prefs[KEY_LOCALE] = locale.name
    }

    override suspend fun setEngineEnabled(enabled: Boolean): AvcrtResult<Unit> = mutate { prefs ->
        prefs[KEY_ENGINE_ENABLED] = enabled
    }

    override suspend fun setAutoEnableOnLaunch(enabled: Boolean): AvcrtResult<Unit> = mutate { prefs ->
        prefs[KEY_AUTO_ENABLE_ON_LAUNCH] = enabled
    }

    override suspend fun setActivePresetId(id: String?): AvcrtResult<Unit> = mutate { prefs ->
        if (id == null) prefs.remove(KEY_ACTIVE_PRESET) else prefs[KEY_ACTIVE_PRESET] = id
    }

    override suspend fun setExperimentalEnabled(enabled: Boolean): AvcrtResult<Unit> = mutate { prefs ->
        prefs[KEY_EXPERIMENTAL] = enabled
    }

    override suspend fun setFloatingWindowEnabled(enabled: Boolean): AvcrtResult<Unit> = mutate { prefs ->
        prefs[KEY_FLOATING_WINDOW] = enabled
    }

    private fun Preferences.toAppSettings(): AppSettings = AppSettings(
        themeMode = this[KEY_THEME_MODE]?.let { runCatching { ThemeMode.valueOf(it) }.getOrNull() }
            ?: ThemeMode.SYSTEM,
        useDynamicColor = this[KEY_USE_DYNAMIC_COLOR] ?: true,
        locale = this[KEY_LOCALE]?.let { runCatching { AppLocale.valueOf(it) }.getOrNull() }
            ?: AppLocale.SYSTEM,
        engineEnabled = this[KEY_ENGINE_ENABLED] ?: false,
        autoEnableOnLaunch = this[KEY_AUTO_ENABLE_ON_LAUNCH] ?: false,
        activePresetId = this[KEY_ACTIVE_PRESET],
        experimentalEnabled = this[KEY_EXPERIMENTAL] ?: false,
        floatingWindowEnabled = this[KEY_FLOATING_WINDOW] ?: false,
    )

    private suspend inline fun mutate(crossinline block: (MutablePreferences) -> Unit): AvcrtResult<Unit> =
        runCatching {
            dataStore.edit { block(it) }
            AvcrtResult.Success(Unit)
        }.getOrElse { AvcrtResult.Failure(AvcrtError.Io("DataStore edit failed", it)) }

    /** Used by Hilt's eager-singletons to pre-warm the snapshot before MainActivity reads it. */
    fun primeBlocking() {
        runBlocking { settingsFlow.value }
    }

    private companion object {
        val KEY_THEME_MODE = stringPreferencesKey("theme_mode")
        val KEY_USE_DYNAMIC_COLOR = booleanPreferencesKey("use_dynamic_color")
        val KEY_LOCALE = stringPreferencesKey("locale")
        val KEY_ENGINE_ENABLED = booleanPreferencesKey("engine_enabled")
        val KEY_AUTO_ENABLE_ON_LAUNCH = booleanPreferencesKey("auto_enable_on_launch")
        val KEY_ACTIVE_PRESET = stringPreferencesKey("active_preset_id")
        val KEY_EXPERIMENTAL = booleanPreferencesKey("experimental_enabled")
        val KEY_FLOATING_WINDOW = booleanPreferencesKey("floating_window_enabled")
    }
}
