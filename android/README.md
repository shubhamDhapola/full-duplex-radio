# Android app

A Kotlin/Compose app that links the project core directly. The core is consumed
with `add_subdirectory`, not vendored or copied, so the phone runs the *same*
source the host benchmarks in [docs/measurements.md](../docs/measurements.md)
measured. If the Android build needed its own fork of the core, every published
figure would describe different code from the one shipping.

Right now the app does one thing: run the core's self-check on the device and
show the result. That is deliberate — see "What this is not" below.

## Build

```bash
cd android
./gradlew :app:assembleDebug
./gradlew :app:installDebug      # with a device attached
adb logcat -s fdradio            # the self-check also goes to logcat
```

## Toolchain

Two things are not the defaults, and both will bite anyone who skips them.

**A JDK.** Gradle needs one, and macOS ships a stub that only tells you to
install Java. Android Studio's bundled runtime works:

```bash
export JAVA_HOME="/Applications/Android Studio.app/Contents/jbr/Contents/Home"
```

**An external CMake.** The Android SDK bundles CMake 3.22.1. The core requires
3.24, and `FetchContent_Declare(... EXCLUDE_FROM_ALL)` in `cmake/opus.cmake`
requires 3.28, so the SDK's copy cannot configure this project at all. Point the
build at a newer one in `local.properties`, which is machine-local and not
committed:

```properties
sdk.dir=/Users/you/Library/Android/sdk
cmake.dir=/opt/homebrew
```

`cmake.dir` is the prefix containing `bin/cmake`; Gradle also looks for `ninja`
beside it. Installing `cmake;3.31.x` through the SDK manager works equally well
and is the better answer on a machine without Homebrew.

Versions are pinned rather than defaulted: Gradle 8.13 in the wrapper, AGP
8.13.1 and Kotlin 2.1.20 in `gradle/libs.versions.toml`, and
`ndkVersion = "29.0.14206865"` in `app/build.gradle.kts`. AGP 8.13 requires
Gradle 8.13 exactly. The NDK is pinned because leaving it unset makes the libc++
the core is compiled against a function of which AGP release you happen to have.

## Decisions worth knowing

**`minSdk 26.`** AAudio arrives in API 26, and AAudio is the only route to the
low-latency audio this project exists to measure. Below it Oboe falls back to
OpenSL ES, which defeats the purpose of the milestone.

**Two ABIs: `arm64-v8a` and `x86_64`.** 64-bit only. `armeabi-v7a` would double
the native build matrix for devices that cannot usefully run the app, and Play
has required 64-bit since 2019. `x86_64` is for the emulator, which can run the
conformance vectors — but its audio path is not representative, so no latency
figure should ever be quoted from it.

**`ANDROID_STL=c++_static`.** One static core archive linked into one `.so`,
and nothing else in the process shares the C++ runtime, so there is no second
library to ship and keep in step.

**The debug build keeps the native half optimised.** A latency number from an
unoptimised build is meaningless, and that is exactly the number someone will
read off the diagnostics screen.

## What this is not

`NativeCore` is not the JNI boundary the audio path will use. That boundary
carries commands and state, must never be crossed from an audio callback, and
arrives with the Oboe work.

Keeping this to a single self-check until then means the first thing running on
the device has an unambiguous failure mode: if `selfCheck()` reports a bad Opus
round trip, the problem is the toolchain or the core — not a threading model
that does not exist yet.
