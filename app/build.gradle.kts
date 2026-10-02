plugins {
    id("com.android.application")
}

android {
    namespace = "dev.framegen.android"
    compileSdk = 35
    ndkVersion = "30.0.16248370"

    defaultConfig {
        applicationId = "dev.framegen.android"
        minSdk = 29
        targetSdk = 35
        versionCode = 1
        versionName = "0.0.3"

        ndk {
            abiFilters += listOf("arm64-v8a")
        }

        externalNativeBuild {
            cmake {
                cppFlags += listOf("-std=c++20", "-Wall", "-Wextra")
            }
        }
    }

    buildFeatures {
        prefab = true
    }

    packaging {
        jniLibs {
            pickFirsts += setOf("**/libbytehook.so", "**/libshadowhook.so")
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }
}

dependencies {
    implementation("com.bytedance:bytehook:1.1.1")
}
