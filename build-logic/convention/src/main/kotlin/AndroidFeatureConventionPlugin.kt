import org.gradle.api.Plugin
import org.gradle.api.Project
import org.gradle.api.artifacts.VersionCatalogsExtension
import org.gradle.kotlin.dsl.dependencies
import org.gradle.kotlin.dsl.getByType

/**
 * Convention plugin for feature modules. Applies android library + compose + hilt
 * and wires in the shared core/domain dependencies that every feature needs.
 *
 * Feature modules should NOT depend on :data directly — they only depend on :domain
 * (repository interfaces) and let the :app module supply the implementation. This is
 * how we keep them swappable and testable.
 */
class AndroidFeatureConventionPlugin : Plugin<Project> {
    override fun apply(target: Project) = with(target) {
        with(pluginManager) {
            apply("avcrt.android.library")
            apply("avcrt.android.compose")
            apply("avcrt.android.hilt")
        }

        val catalog = extensions.getByType<VersionCatalogsExtension>().named("libs")

        dependencies {
            add("implementation", project(":core:common"))
            add("implementation", project(":core:ui"))
            add("implementation", project(":domain"))

            add("implementation", catalog.findLibrary("androidx-core-ktx").orElseThrow())
            add("implementation", catalog.findLibrary("androidx-activity-compose").orElseThrow())
            add("implementation", catalog.findLibrary("androidx-navigation-compose").orElseThrow())
            add("implementation", catalog.findLibrary("hilt-navigation-compose").orElseThrow())
            add("implementation", catalog.findLibrary("timber").orElseThrow())

            catalog.findBundle("lifecycle").orElseThrow().get().forEach {
                add("implementation", it)
            }
        }
    }
}
