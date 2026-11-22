plugins {
    alias(libs.plugins.avcrt.android.feature)
}

android {
    namespace = "dev.avcrt.feature.files"
}

dependencies {
    implementation(libs.kotlinx.coroutines.android)
}
