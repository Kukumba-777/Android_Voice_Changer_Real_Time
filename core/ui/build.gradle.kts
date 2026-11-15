plugins {
    alias(libs.plugins.avcrt.android.library)
    alias(libs.plugins.avcrt.android.compose)
}

android {
    namespace = "dev.avcrt.core.ui"
}

dependencies {
    api(project(":core:common"))
    api(project(":domain"))

    api(libs.androidx.core.ktx)
    api(libs.androidx.appcompat)
    api(libs.bundles.lifecycle)
}
