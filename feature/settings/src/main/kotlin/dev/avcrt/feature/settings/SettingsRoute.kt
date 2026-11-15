package dev.avcrt.feature.settings

import androidx.navigation.NavController
import androidx.navigation.NavGraphBuilder
import androidx.navigation.compose.composable

object SettingsRoute {
    const val path = "settings"
}

fun NavGraphBuilder.settingsGraph(navController: NavController) {
    composable(SettingsRoute.path) {
        SettingsScreen(navController = navController)
    }
}
