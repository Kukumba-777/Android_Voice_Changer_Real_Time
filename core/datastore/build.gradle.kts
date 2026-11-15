plugins {
    alias(libs.plugins.avcrt.android.library)
    alias(libs.plugins.avcrt.android.hilt)
}

android {
    namespace = "dev.avcrt.core.datastore"
}

dependencies {
    api(libs.androidx.datastore.preferences)
    implementation(libs.androidx.core.ktx)
    implementation(libs.kotlinx.coroutines.android)
}
