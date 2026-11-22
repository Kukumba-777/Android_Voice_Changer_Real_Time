package dev.avcrt.app.navigation

import androidx.annotation.StringRes
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.outlined.Article
import androidx.compose.material.icons.automirrored.rounded.Article
import androidx.compose.material.icons.outlined.AutoAwesome
import androidx.compose.material.icons.outlined.Equalizer
import androidx.compose.material.icons.outlined.GraphicEq
import androidx.compose.material.icons.outlined.Folder
import androidx.compose.material.icons.outlined.Settings
import androidx.compose.material.icons.rounded.AutoAwesome
import androidx.compose.material.icons.rounded.Equalizer
import androidx.compose.material.icons.rounded.GraphicEq
import androidx.compose.material.icons.rounded.Folder
import androidx.compose.material.icons.rounded.Settings
import androidx.compose.ui.graphics.vector.ImageVector
import dev.avcrt.app.R
import dev.avcrt.feature.converter.ConverterRoute
import dev.avcrt.feature.files.FilesRoute
import dev.avcrt.feature.home.HomeRoute
import dev.avcrt.feature.logs.LogsRoute
import dev.avcrt.feature.presets.PresetsRoute
import dev.avcrt.feature.settings.SettingsRoute

/**
 * Top-level destinations shown in the bottom navigation bar.
 *
 * Adding a new tab is purely additive — register it here and wire its NavGraph
 * extension in [AvcrtNavGraph]; nothing else needs to know.
 */
enum class TopDestination(
    val route: String,
    @StringRes val labelRes: Int,
    val selectedIcon: ImageVector,
    val unselectedIcon: ImageVector,
) {
    HOME(
        route = HomeRoute.path,
        labelRes = R.string.nav_home,
        selectedIcon = Icons.Rounded.GraphicEq,
        unselectedIcon = Icons.Outlined.GraphicEq,
    ),
    PRESETS(
        route = PresetsRoute.path,
        labelRes = R.string.nav_presets,
        selectedIcon = Icons.Rounded.Equalizer,
        unselectedIcon = Icons.Outlined.Equalizer,
    ),
    CONVERTER(
        route = ConverterRoute.path,
        labelRes = R.string.nav_converter,
        selectedIcon = Icons.Rounded.AutoAwesome,
        unselectedIcon = Icons.Outlined.AutoAwesome,
    ),
    FILES(
        route = FilesRoute.path,
        labelRes = R.string.nav_files,
        selectedIcon = Icons.Rounded.Folder,
        unselectedIcon = Icons.Outlined.Folder,
    ),
    SETTINGS(
        route = SettingsRoute.path,
        labelRes = R.string.nav_settings,
        selectedIcon = Icons.Rounded.Settings,
        unselectedIcon = Icons.Outlined.Settings,
    ),
    LOGS(
        route = LogsRoute.path,
        labelRes = R.string.nav_logs,
        selectedIcon = Icons.AutoMirrored.Rounded.Article,
        unselectedIcon = Icons.AutoMirrored.Outlined.Article,
    );

    companion object {
        fun fromRoute(route: String?): TopDestination? =
            entries.firstOrNull { it.route == route }
    }
}
