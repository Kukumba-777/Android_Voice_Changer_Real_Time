# Keep Kotlinx Serialization companion serializers
-keepattributes *Annotation*, InnerClasses
-dontnote kotlinx.serialization.SerializationKt
-keep,includedescriptorclasses class dev.avcrt.**$$serializer { *; }
-keepclassmembers class dev.avcrt.** {
    *** Companion;
}
-keepclasseswithmembers class dev.avcrt.** {
    kotlinx.serialization.KSerializer serializer(...);
}

# Hilt
-keep class dagger.hilt.** { *; }
-keep class * extends dagger.hilt.android.HiltAndroidApp
-keep class * extends androidx.lifecycle.ViewModel
