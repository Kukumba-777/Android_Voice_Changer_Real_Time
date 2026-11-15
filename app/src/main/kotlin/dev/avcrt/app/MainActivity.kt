package dev.avcrt.app

import android.os.Bundle
import android.provider.Settings
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.appcompat.app.AppCompatActivity
import androidx.compose.runtime.getValue
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.lifecycleScope
import dagger.hilt.android.AndroidEntryPoint
import dev.avcrt.app.navigation.AvcrtRootScreen
import dev.avcrt.app.overlay.OverlayService
import dev.avcrt.core.ui.theme.AvcrtTheme
import dev.avcrt.domain.settings.SettingsRepository
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.launch
import javax.inject.Inject

@AndroidEntryPoint
class MainActivity : AppCompatActivity() {

    @Inject lateinit var settingsRepository: SettingsRepository

    override fun onCreate(savedInstanceState: Bundle?) {
        // Replace the splash theme set in the manifest with the real Material You theme
        // before we call setContentView so we don't flash the wrong colors.
        setTheme(R.style.Theme_AVCRT)
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()

        // Drive the floating-overlay service from the persisted flag. Starts when the user enables it
        // (the Home toggle has already secured the overlay permission); stops when disabled. The
        // service is foreground, so it keeps running after the activity goes to the background.
        lifecycleScope.launch {
            settingsRepository.observe()
                .map { it.floatingWindowEnabled }
                .distinctUntilChanged()
                .collect { enabled ->
                    if (enabled && Settings.canDrawOverlays(this@MainActivity)) {
                        OverlayService.start(this@MainActivity)
                    } else if (!enabled) {
                        OverlayService.stop(this@MainActivity)
                    }
                }
        }

        setContent {
            val settings by settingsRepository.observe().collectAsStateWithLifecycle(
                initialValue = settingsRepository.snapshot(),
            )

            AvcrtTheme(
                themeMode = settings.themeMode,
                useDynamicColor = settings.useDynamicColor,
            ) {
                AvcrtRootScreen()
            }
        }
    }
}
