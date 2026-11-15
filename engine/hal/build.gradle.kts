plugins {
    alias(libs.plugins.avcrt.android.library)
    alias(libs.plugins.avcrt.android.hilt)
    alias(libs.plugins.kotlin.serialization)
}

// `libs` in a build.gradle.kts is the type-safe accessor, so we read versions
// straight off it rather than via VersionCatalog.findVersion(...).
val moduleVersion     = libs.versions.moduleVersion.get()
val moduleVersionCode = libs.versions.moduleVersionCode.get()

android {
    namespace = "dev.avcrt.engine.hal"

    buildFeatures {
        buildConfig = true
    }

    defaultConfig {
        // Mirrors :ksu-module's module.prop so BundledModuleMeta always
        // reflects exactly what got bundled into the APK assets — the
        // version catalog is the single source of truth.
        buildConfigField("String", "BUNDLED_MODULE_VERSION", "\"$moduleVersion\"")
        buildConfigField("long", "BUNDLED_MODULE_VERSION_CODE", "${moduleVersionCode}L")
    }
}

dependencies {
    api(project(":engine:api"))

    implementation(project(":core:common"))
    implementation(project(":core:root"))
    implementation(project(":domain"))

    implementation(libs.androidx.core.ktx)
    implementation(libs.kotlinx.coroutines.android)
    implementation(libs.kotlinx.serialization.json)
    implementation(libs.timber)
}
