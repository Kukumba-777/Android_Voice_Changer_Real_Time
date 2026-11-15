package dev.avcrt.feature.home

import dev.avcrt.core.root.RootStatus
import dev.avcrt.domain.engine.EngineState

data class HomeUiState(
    val engine: EngineState = EngineState.Idle,
    val rootStatus: RootStatus = RootStatus.Unknown,
    val isToggling: Boolean = false,
    val isInstallingModule: Boolean = false,
    val transientMessage: String? = null,
    val floatingWindowEnabled: Boolean = false,
)
