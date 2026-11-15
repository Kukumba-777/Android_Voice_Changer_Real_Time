plugins {
    alias(libs.plugins.avcrt.android.library)
    alias(libs.plugins.kotlin.serialization)
}

android {
    namespace = "dev.avcrt.engine.api"
}

dependencies {
    api(project(":domain"))
    api(libs.kotlinx.serialization.json)
}
