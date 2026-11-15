package dev.avcrt.app.engine

import dev.avcrt.core.common.dispatcher.DispatcherProvider
import dev.avcrt.core.common.startup.Initializer
import dev.avcrt.domain.engine.VoiceEnginePort
import dev.avcrt.domain.settings.SettingsRepository
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch
import timber.log.Timber
import javax.inject.Inject

/**
 * Optional “turn the engine on whenever the app launches”.
 *
 * Driven by [dev.avcrt.domain.settings.AppSettings.autoEnableOnLaunch]. Stays
 * a no-op when the setting is off, so the default cold start does NOT prompt
 * for root.
 *
 * Note: when enabled, this fires a one-shot `engine.start()` coroutine —
 * blocking the main thread waiting for root is forbidden. If root is denied
 * the engine simply stays in its previous state and the user can retry
 * manually from Home.
 */
class EngineAutoStartInitializer @Inject constructor(
    private val settings: SettingsRepository,
    private val engine: VoiceEnginePort,
    private val dispatchers: DispatcherProvider,
) : Initializer {

    override val priority: Int get() = 50

    override fun initialize() {
        val snapshot = settings.snapshot()
        if (!snapshot.autoEnableOnLaunch) return
        if (snapshot.engineEnabled) return  // already on; nothing to do
        Timber.i("autoEnableOnLaunch is on — kicking engine.start()")
        CoroutineScope(SupervisorJob() + dispatchers.io).launch {
            engine.start()
        }
    }
}
