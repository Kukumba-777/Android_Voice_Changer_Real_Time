package dev.avcrt.feature.settings

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import dagger.hilt.android.lifecycle.HiltViewModel
import dev.avcrt.core.root.RootAccess
import dev.avcrt.core.root.RootStatus
import dev.avcrt.domain.settings.AppLocale
import dev.avcrt.domain.settings.AppSettings
import dev.avcrt.domain.settings.SettingsRepository
import dev.avcrt.domain.settings.ThemeMode
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
import javax.inject.Inject

data class SettingsUiState(
    val settings: AppSettings = AppSettings(),
    val rootStatus: RootStatus = RootStatus.Unknown,
)

@HiltViewModel
class SettingsViewModel @Inject constructor(
    private val settings: SettingsRepository,
    rootAccess: RootAccess,
) : ViewModel() {

    val state: StateFlow<SettingsUiState> = combine(
        settings.observe(),
        rootAccess.status,
    ) { s, root -> SettingsUiState(s, root) }
        .stateIn(viewModelScope, SharingStarted.WhileSubscribed(5_000L), SettingsUiState())

    fun onThemeMode(mode: ThemeMode) {
        viewModelScope.launch { settings.setThemeMode(mode) }
    }

    fun onDynamicColor(enabled: Boolean) {
        viewModelScope.launch { settings.setUseDynamicColor(enabled) }
    }

    fun onLocale(locale: AppLocale) {
        viewModelScope.launch { settings.setLocale(locale) }
    }

    fun onAutoEnableOnLaunch(enabled: Boolean) {
        viewModelScope.launch { settings.setAutoEnableOnLaunch(enabled) }
    }

    fun onExperimentalEnabled(enabled: Boolean) {
        viewModelScope.launch { settings.setExperimentalEnabled(enabled) }
    }
}
