// Android Modern UI (Kotlin + Jetpack Compose): Gradle project driven by tools/build.py android
// modern (gradlew assembleRelease). Plugin and library versions are pinned in
// gradle/libs.versions.toml.
pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
    }
}

rootProject.name = "loomora-android-compose"
include(":app")
