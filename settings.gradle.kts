pluginManagement {
    includeBuild("build-logic")
    repositories {
        google {
            content {
                includeGroupByRegex("com\\.android.*")
                includeGroupByRegex("com\\.google.*")
                includeGroupByRegex("androidx.*")
            }
        }
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
        maven("https://jitpack.io")
    }
}

rootProject.name = "AVCRT"

include(":app")

include(":core:common")
include(":core:ui")
include(":core:datastore")
include(":core:root")

include(":domain")
include(":data")

include(":feature:home")
include(":feature:presets")
include(":feature:logs")
include(":feature:settings")
include(":feature:converter")
include(":feature:files")

include(":engine:api")
include(":engine:hal")
include(":engine:rvc")

include(":ksu-module")
