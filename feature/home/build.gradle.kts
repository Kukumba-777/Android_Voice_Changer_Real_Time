plugins {
    alias(libs.plugins.avcrt.android.feature)
}

android {
    namespace = "dev.avcrt.feature.home"
}

dependencies {
    implementation(project(":core:root"))
}
