plugins {
    alias(libs.plugins.avcrt.android.library)
    alias(libs.plugins.avcrt.android.hilt)
    alias(libs.plugins.kotlin.serialization)
}

android {
    namespace = "dev.avcrt.core.common"
    buildFeatures { buildConfig = true }
    defaultConfig {
        buildConfigField("String", "MODULE_ID", "\"${libs.versions.moduleId.get()}\"")
    }
}

dependencies {
    api(libs.kotlinx.coroutines.core)
    api(libs.kotlinx.coroutines.android)
    api(libs.kotlinx.serialization.json)
    api(libs.timber)
}
