plugins {
    alias(libs.plugins.avcrt.android.library)
    alias(libs.plugins.avcrt.android.hilt)
}

android {
    namespace = "dev.avcrt.engine.rvc"
}

dependencies {
    implementation(project(":core:common"))
    implementation(project(":domain"))

    // On-device neural inference (ONNX Runtime Mobile) — the RVC pipeline runs here.
    implementation(libs.onnxruntime.android)

    implementation(libs.kotlinx.coroutines.android)
    implementation(libs.timber)
}
