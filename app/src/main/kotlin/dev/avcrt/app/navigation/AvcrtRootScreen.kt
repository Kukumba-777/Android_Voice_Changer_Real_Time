package dev.avcrt.app.navigation

import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Icon
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.navigation.NavGraph.Companion.findStartDestination
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.currentBackStackEntryAsState
import androidx.navigation.compose.rememberNavController
import dev.avcrt.feature.converter.converterGraph
import dev.avcrt.feature.home.homeGraph
import dev.avcrt.feature.logs.logsGraph
import dev.avcrt.feature.presets.presetsGraph
import dev.avcrt.feature.settings.settingsGraph

@Composable
fun AvcrtRootScreen() {
    val navController = rememberNavController()

    Scaffold(
        bottomBar = {
            val backStackEntry by navController.currentBackStackEntryAsState()
            val currentRoute = backStackEntry?.destination?.route
            val currentTop = TopDestination.fromRoute(currentRoute)

            NavigationBar {
                TopDestination.entries.forEach { destination ->
                    val selected = destination == currentTop
                    NavigationBarItem(
                        selected = selected,
                        icon = {
                            Icon(
                                imageVector = if (selected) destination.selectedIcon else destination.unselectedIcon,
                                contentDescription = null,
                            )
                        },
                        label = { Text(stringResource(destination.labelRes)) },
                        onClick = {
                            navController.navigate(destination.route) {
                                popUpTo(navController.graph.findStartDestination().id) {
                                    saveState = true
                                }
                                launchSingleTop = true
                                restoreState = true
                            }
                        },
                    )
                }
            }
        },
    ) { innerPadding ->
        NavHost(
            navController = navController,
            startDestination = TopDestination.HOME.route,
            modifier = Modifier.padding(innerPadding),
        ) {
            // Every feature module contributes its own subgraph extension.
            // Adding a feature = one new call site here + entry in TopDestination.
            homeGraph(navController)
            presetsGraph(navController)
            converterGraph(navController)
            logsGraph(navController)
            settingsGraph(navController)
        }
    }
}
