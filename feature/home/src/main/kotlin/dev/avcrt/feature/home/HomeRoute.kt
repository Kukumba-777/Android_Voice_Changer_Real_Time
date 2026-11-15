package dev.avcrt.feature.home

import androidx.navigation.NavController
import androidx.navigation.NavGraphBuilder
import androidx.navigation.compose.composable

object HomeRoute {
    const val path = "home"
}

fun NavGraphBuilder.homeGraph(navController: NavController) {
    composable(HomeRoute.path) {
        HomeScreen(navController = navController)
    }
}
