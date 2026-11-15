plugins {
    alias(libs.plugins.avcrt.android.library)
    alias(libs.plugins.avcrt.android.hilt)
    alias(libs.plugins.kotlin.serialization)
}

android {
    namespace = "dev.avcrt.core.common"
}

dependencies {
    api(libs.kotlinx.coroutines.core)
    api(libs.kotlinx.coroutines.android)
    api(libs.kotlinx.serialization.json)
    api(libs.timber)
}
