package dev.avcrt.core.common.startup

/**
 * Anything wired into the multibinding `Set<Initializer>` runs at app launch.
 * Lower priority numbers run first. Keep work minimal — heavy work belongs
 * in a coroutine launched by the initializer, not in [initialize] itself.
 */
interface Initializer {
    val priority: Int get() = 100
    fun initialize()
}
