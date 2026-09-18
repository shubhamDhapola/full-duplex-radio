plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.android)
    alias(libs.plugins.kotlin.compose)
}

android {
    namespace = "dev.fdradio"
    compileSdk = 36

    // Pinned, not defaulted. AGP's default NDK is whatever that AGP release
    // shipped against, so leaving it unset makes the native ABI of the app a
    // function of the build tooling rather than a decision -- and a silent NDK
    // bump changes the libc++ the core is compiled against.
    ndkVersion = "29.0.14206865"

    defaultConfig {
        applicationId = "dev.fdradio"

        // API 26 is where AAudio arrives, and AAudio is the only path to the
        // low-latency audio this project is about. Oboe falls back to OpenSL ES
        // below it, which defeats the point of the milestone.
        minSdk = 26
        targetSdk = 36
        versionCode = 1
        versionName = "0.1"

        // 64-bit only. armeabi-v7a would double the native build matrix for
        // devices that cannot run the app usefully anyway, and Play has
        // required 64-bit since 2019. x86_64 is here for the emulator, which
        // can run the conformance vectors even though its audio path is not
        // representative enough to take latency figures from.
        ndk { abiFilters += listOf("arm64-v8a", "x86_64") }

        externalNativeBuild {
            cmake {
                arguments += listOf(
                    "-DRADIO_WITH_OPUS=ON",
                    // The core is one static archive linked into one .so, so
                    // the static C++ runtime is the right choice: nothing else
                    // in the process needs to share it, and c++_shared would
                    // add a second .so to ship and to keep in step.
                    "-DANDROID_STL=c++_static",
                )
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            // No `version` here on purpose. The SDK's bundled CMake is 3.22.1,
            // and the core needs 3.24 (and 3.28 for FetchContent's
            // EXCLUDE_FROM_ALL), so the build uses an external CMake pointed at
            // by cmake.dir in local.properties. See android/README.md.
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro",
            )
        }
        debug {
            // The core is measured, not guessed at, so the native half is
            // optimised even in a debug build. A debug-optimisation-level
            // latency number is meaningless, and that is the number someone
            // will read off the diagnostics screen.
            isJniDebuggable = true
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }

    buildFeatures {
        compose = true
    }
}

dependencies {
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.lifecycle.runtime.ktx)
    implementation(libs.androidx.activity.compose)

    implementation(platform(libs.androidx.compose.bom))
    implementation(libs.androidx.ui)
    implementation(libs.androidx.ui.graphics)
    implementation(libs.androidx.ui.tooling.preview)
    implementation(libs.androidx.material3)

    debugImplementation(libs.androidx.ui.tooling)
}
