package dev.avcrt.app.locale

import androidx.appcompat.app.AppCompatDelegate
import androidx.core.os.LocaleListCompat
import dev.avcrt.domain.settings.AppLocale
import dev.avcrt.domain.settings.SettingsRepository
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.launchIn
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.onEach
import javax.inject.Inject
import javax.inject.Singleton

/**
 * Bridges the persisted [AppLocale] setting to AndroidX's per-app locale system.
 *
 * AppCompatDelegate handles both Android 13+ (native per-app locale) and older
 * releases (configuration override + activity recreate), so we don't have to
 * branch on SDK version here.
 */
@Singleton
class LocaleApplier @Inject constructor(
    private val settings: SettingsRepository,
) {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)

    fun start() {
        // Apply the current value immediately so the first frame draws in the right language.
        apply(settings.snapshot().locale)

        settings.observe()
            .map { it.locale }
            .distinctUntilChanged()
            .onEach(::apply)
            .launchIn(scope)
    }

    private fun apply(locale: AppLocale) {
        val tags = locale.tag
        AppCompatDelegate.setApplicationLocales(
            if (tags == null) LocaleListCompat.getEmptyLocaleList()
            else LocaleListCompat.forLanguageTags(tags),
        )
    }
}
