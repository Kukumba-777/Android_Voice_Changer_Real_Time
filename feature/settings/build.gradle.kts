plugins {
    alias(libs.plugins.avcrt.android.feature)
}

android {
    namespace = "dev.avcrt.feature.settings"
}

dependencies {
    implementation(project(":core:root"))
}
