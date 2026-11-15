package dev.avcrt.app

import dev.avcrt.core.common.startup.Initializer
import javax.inject.Inject

/**
 * Aggregates per-feature [Initializer]s and runs them at app startup.
 *
 * Hilt collects every binding registered via `@IntoSet` into [initializers].
 * Each feature module can contribute its own without touching this class.
 */
class AppInitializers @Inject constructor(
    private val initializers: Set<@JvmSuppressWildcards Initializer>,
) {
    fun runAll() {
        initializers
            .sortedBy { it.priority }
            .forEach { it.initialize() }
    }
}
