package dev.avcrt.feature.home

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import dagger.hilt.android.lifecycle.HiltViewModel
import dev.avcrt.core.common.result.AvcrtResult
import dev.avcrt.core.common.result.onFailure
import dev.avcrt.core.root.RootAccess
import dev.avcrt.domain.engine.VoiceEnginePort
import dev.avcrt.domain.settings.SettingsRepository
import dev.avcrt.domain.usecase.ObserveEngineUseCase
import dev.avcrt.domain.usecase.ToggleEngineUseCase
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.launchIn
import kotlinx.coroutines.flow.onEach
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
import timber.log.Timber
import javax.inject.Inject

@HiltViewModel
class HomeViewModel @Inject constructor(
    private val engine: VoiceEnginePort,
    private val toggleEngine: ToggleEngineUseCase,
    private val rootAccess: RootAccess,
    private val settings: SettingsRepository,
    observeEngine: ObserveEngineUseCase,
) : ViewModel() {

    private val transient = MutableStateFlow<String?>(null)
    private val toggling = MutableStateFlow(false)
    private val installing = MutableStateFlow(false)

    val state: StateFlow<HomeUiState> = combine(
        observeEngine(),
        rootAccess.status,
        transient,
        toggling,
        installing,
    ) { engine, root, msg, isToggling, isInstalling ->
        HomeUiState(
            engine = engine,
            rootStatus = root,
            isToggling = isToggling,
            isInstallingModule = isInstalling,
            transientMessage = msg,
        )
    }.combine(settings.observe()) { base, s ->
        base.copy(floatingWindowEnabled = s.floatingWindowEnabled)
    }.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5_000L), HomeUiState())

    init {
        // Probe root once when the screen first attaches.
        viewModelScope.launch {
            rootAccess.requestAccess()
            engine.refreshState()
        }
        engine.observeState()
            .onEach { Timber.tag(TAG).i("engine -> %s", it) }
            .launchIn(viewModelScope)
    }

    fun onToggleEngine(enable: Boolean) {
        viewModelScope.launch {
            toggling.value = true
            toggleEngine(enable).onFailure { err ->
                transient.value = err.message
            }
            toggling.value = false
        }
    }

    fun onRequestRoot() {
        viewModelScope.launch {
            rootAccess.requestAccess().onFailure { err ->
                transient.value = err.message
            }
            engine.refreshState()
        }
    }

    fun onInstallModule(withoutZygisk: Boolean = false) {
        if (installing.value) return
        viewModelScope.launch {
            installing.value = true
            when (val res = engine.installModule(withoutZygisk)) {
                is AvcrtResult.Success -> engine.refreshState()
                is AvcrtResult.Failure -> transient.value = res.error.message
            }
            installing.value = false
        }
    }

    fun onReboot() {
        viewModelScope.launch {
            engine.reboot().onFailure { err ->
                transient.value = err.message
            }
        }
    }

    /**
     * Persists the floating-window preference. The app layer (MainActivity) observes this flag and
     * starts/stops the overlay service; the Home screen handles the overlay PERMISSION before
     * calling this with `true`.
     */
    fun onSetFloatingWindow(enabled: Boolean) {
        viewModelScope.launch { settings.setFloatingWindowEnabled(enabled) }
    }

    fun consumeTransient() {
        transient.value = null
    }

    private companion object {
        const val TAG = "HomeVM"
    }
}
