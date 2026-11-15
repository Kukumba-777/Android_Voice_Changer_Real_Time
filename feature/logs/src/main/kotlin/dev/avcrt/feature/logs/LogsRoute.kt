package dev.avcrt.feature.logs

import androidx.navigation.NavController
import androidx.navigation.NavGraphBuilder
import androidx.navigation.compose.composable

object LogsRoute {
    const val path = "logs"
}

fun NavGraphBuilder.logsGraph(navController: NavController) {
    composable(LogsRoute.path) {
        LogsScreen(navController = navController)
    }
}
