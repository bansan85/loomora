plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.compose)
}

// Signing: tools/build.py generates a throwaway key outside the repository and passes it with
// -Ploomora.keystore=<file> (-Ploomora.storepass, -Ploomora.keyalias). Without it the release APK
// is built unsigned.
val loomoraKeystore = providers.gradleProperty("loomora.keystore").orNull

android {
    namespace = "fr.bansan.loomora.probe"
    compileSdk = 37
    ndkVersion = "30.0.16248370"

    defaultConfig {
        // Different from the Legacy UI (fr.bansan.loomora.probe) so both can be installed together; the
        // Kotlin class is still fr.bansan.loomora.probe.Native (the JNI glue exports its functions).
        applicationId = "fr.bansan.loomora.probe.modern"
        // 24 is the decided floor; -Ploomora.minSdk=21 only serves spike S8 (does Compose accept 21).
        minSdk = (providers.gradleProperty("loomora.minSdk").orNull ?: "24").toInt()
        targetSdk = 37
        versionCode = 1
        versionName = "1.0"
        ndk {
            abiFilters += listOf("armeabi-v7a", "arm64-v8a", "x86", "x86_64")
        }
        externalNativeBuild {
            cmake {
                arguments += listOf("-DANDROID_STL=c++_static", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON")
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("CMakeLists.txt")
            version = "3.22.1"
        }
    }

    signingConfigs {
        if (loomoraKeystore != null) {
            create("release") {
                storeFile = file(loomoraKeystore)
                storePassword = providers.gradleProperty("loomora.storepass").get()
                keyAlias = providers.gradleProperty("loomora.keyalias").get()
                keyPassword = providers.gradleProperty("loomora.storepass").get()
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
            if (loomoraKeystore != null) {
                signingConfig = signingConfigs.getByName("release")
            }
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    buildFeatures {
        compose = true
        buildConfig = false
    }

    packaging {
        resources.excludes += "/META-INF/{AL2.0,LGPL2.1}"
    }
}

dependencies {
    implementation(platform(libs.compose.bom))
    implementation(libs.compose.ui)
    implementation(libs.compose.foundation)
    implementation(libs.compose.material3)
    implementation(libs.androidx.activity.compose)
}
