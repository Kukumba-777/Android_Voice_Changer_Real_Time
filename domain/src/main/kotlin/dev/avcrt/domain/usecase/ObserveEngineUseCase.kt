package dev.avcrt.domain.usecase

import dev.avcrt.domain.engine.EngineState
import dev.avcrt.domain.engine.VoiceEnginePort
import kotlinx.coroutines.flow.Flow
import javax.inject.Inject

class ObserveEngineUseCase @Inject constructor(
    private val engine: VoiceEnginePort,
) {
    operator fun invoke(): Flow<EngineState> = engine.observeState()
}
