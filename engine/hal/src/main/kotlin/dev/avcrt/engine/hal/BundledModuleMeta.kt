package dev.avcrt.engine.hal

/**
 * Version coords of the KSU module the *current* APK ships with.
 *
 * Both this file and the module's `module.prop` are sourced from the same
 * entries in `gradle/libs.versions.toml` — this side via [BuildConfig], the
 * module side via the `writeModuleProp` task — so they cannot drift apart.
 */
object BundledModuleMeta {
    const val VERSION: String = BuildConfig.BUNDLED_MODULE_VERSION
    const val VERSION_CODE: Long = BuildConfig.BUNDLED_MODULE_VERSION_CODE
}
