package dev.avcrt.domain.settings

import dev.avcrt.core.common.result.AvcrtResult
import kotlinx.coroutines.flow.Flow

interface SettingsRepository {

    fun observe(): Flow<AppSettings>

    /**
     * Cheap synchronous read of the last cached settings. Use only when a Flow
     * is impractical (e.g. setting the initial Compose state before the first
     * Flow emission lands).
     */
    fun snapshot(): AppSettings

    suspend fun setThemeMode(mode: ThemeMode): AvcrtResult<Unit>
    suspend fun setUseDynamicColor(enabled: Boolean): AvcrtResult<Unit>
    suspend fun setLocale(locale: AppLocale): AvcrtResult<Unit>
    suspend fun setEngineEnabled(enabled: Boolean): AvcrtResult<Unit>
    suspend fun setAutoEnableOnLaunch(enabled: Boolean): AvcrtResult<Unit>
    suspend fun setActivePresetId(id: String?): AvcrtResult<Unit>
    suspend fun setExperimentalEnabled(enabled: Boolean): AvcrtResult<Unit>
    suspend fun setFloatingWindowEnabled(enabled: Boolean): AvcrtResult<Unit>
}
