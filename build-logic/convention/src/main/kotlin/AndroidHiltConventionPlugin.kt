import org.gradle.api.Plugin
import org.gradle.api.Project
import org.gradle.api.artifacts.VersionCatalogsExtension
import org.gradle.kotlin.dsl.dependencies
import org.gradle.kotlin.dsl.getByType

class AndroidHiltConventionPlugin : Plugin<Project> {
    override fun apply(target: Project) = with(target) {
        with(pluginManager) {
            apply("com.google.devtools.ksp")
            apply("com.google.dagger.hilt.android")
        }

        val catalog = extensions.getByType<VersionCatalogsExtension>().named("libs")
        val hiltAndroid = catalog.findLibrary("hilt-android").orElseThrow()
        val hiltCompiler = catalog.findLibrary("hilt-compiler").orElseThrow()

        dependencies {
            add("implementation", hiltAndroid)
            add("ksp", hiltCompiler)
        }
    }
}
