package dev.avcrt.domain.settings

import kotlinx.serialization.Serializable

@Serializable
data class AppSettings(
    val themeMode: ThemeMode = ThemeMode.SYSTEM,
    val useDynamicColor: Boolean = true,
    val locale: AppLocale = AppLocale.SYSTEM,
    /**
     * The user’s most-recent intent for the master switch on Home. Survives
     * process death but is NOT the same as “auto-start on launch” — see
     * [autoEnableOnLaunch] for that.
     */
    val engineEnabled: Boolean = false,
    /**
     * If true, the app starts the engine on cold launch regardless of what
     * [engineEnabled] was when the app was last killed. Lives in the Settings
     * tab; toggling the Home master switch must NOT change this.
     */
    val autoEnableOnLaunch: Boolean = false,
    val activePresetId: String? = null,
    /**
     * Unlocks the hidden "experimental / dangerous" effects (FFT spectral, auto-tune, harmonizer) in
     * the custom-preset editor. Off by default — these add latency (bad on calls) and can distort.
     */
    val experimentalEnabled: Boolean = false,
    /**
     * Shows a draggable floating overlay bubble with quick controls (voice on/off, preset picker,
     * soundboard) over other apps. Requires the SYSTEM_ALERT_WINDOW ("draw over other apps")
     * permission; the Home toggle guides the user to grant it. Off by default.
     */
    val floatingWindowEnabled: Boolean = false,
)

enum class ThemeMode { SYSTEM, LIGHT, DARK }

enum class AppLocale(val tag: String?) {
    /** Use whatever locale the system reports. */
    SYSTEM(null),
    EN("en"),
    RU("ru"),
}
