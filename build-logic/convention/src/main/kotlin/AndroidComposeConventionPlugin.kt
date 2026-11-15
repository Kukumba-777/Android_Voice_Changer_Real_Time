import com.android.build.api.dsl.CommonExtension
import org.gradle.api.Plugin
import org.gradle.api.Project
import org.gradle.api.artifacts.VersionCatalogsExtension
import org.gradle.kotlin.dsl.configure
import org.gradle.kotlin.dsl.dependencies
import org.gradle.kotlin.dsl.getByType

class AndroidComposeConventionPlugin : Plugin<Project> {
    override fun apply(target: Project) = with(target) {
        pluginManager.apply("org.jetbrains.kotlin.plugin.compose")

        val ext = extensions.findByName("android") as? CommonExtension<*, *, *, *, *, *>
            ?: error("Compose convention plugin requires the Android plugin to be applied first.")

        ext.buildFeatures.compose = true

        val catalog = extensions.getByType<VersionCatalogsExtension>().named("libs")
        val composeBom = catalog.findLibrary("compose-bom").orElseThrow()
        val composeBundle = catalog.findBundle("compose-ui").orElseThrow()
        val composeTooling = catalog.findLibrary("compose-ui-tooling").orElseThrow()

        dependencies {
            add("implementation", platform(composeBom))
            add("androidTestImplementation", platform(composeBom))
            add("implementation", composeBundle)
            add("debugImplementation", composeTooling)
        }
    }
}
