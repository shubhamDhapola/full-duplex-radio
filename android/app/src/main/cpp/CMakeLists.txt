# The Android native library: the project core, plus a JNI shim over it.
#
# The core is consumed by add_subdirectory rather than by being copied or
# vendored, so the phone links the *same* source the host benchmarks measured.
# That is the whole reason the core has no platform dependencies beyond POSIX
# sockets -- if the Android build needed its own fork of it, every number in
# docs/measurements.md would describe different code from the one shipping.
cmake_minimum_required(VERSION 3.24)
project(fdradio_jni LANGUAGES C CXX)

# android/app/src/main/cpp -> repository root.
get_filename_component(RADIO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.."
                       ABSOLUTE)

if(NOT EXISTS "${RADIO_ROOT}/core/CMakeLists.txt")
    message(FATAL_ERROR
        "Cannot find the project core at ${RADIO_ROOT}. The Android project "
        "expects to live at <repo>/android and reach the core by relative "
        "path.")
endif()

# The root build switches tests and tools off when cross-compiling, so this
# brings in radio_core and nothing else.
add_subdirectory("${RADIO_ROOT}" "${CMAKE_BINARY_DIR}/radio_core")

# Oboe: the Android audio layer. Platform-specific by definition, so it is
# fetched here rather than by the core, which stays free of anything that is not
# POSIX.
include("${CMAKE_CURRENT_SOURCE_DIR}/../../../../cmake/oboe.cmake")

add_library(fdradio_jni SHARED
    jni_bridge.cpp
    audio_engine.cpp
)

# C++20 arrives with radio_core, which declares it as a usage requirement --
# this target is in the parent scope of the core's add_subdirectory, so the
# root build's CMAKE_CXX_STANDARD does not reach it.
target_link_libraries(fdradio_jni PRIVATE
    radio_core
    oboe
    android
    log
)

# The JNI shim is ours, so it is held to the same warning policy as the core.
target_link_libraries(fdradio_jni PRIVATE radio_warnings)
