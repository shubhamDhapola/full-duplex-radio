cmake_minimum_required(VERSION 3.24)
project(full_duplex_radio LANGUAGES C CXX)

# C++20 rather than 17. Two of the things this project leans on are C++20:
# std::span (a non-owning view is exactly the right shape for a parsed packet
# that must not copy its payload) and the guarantee that signed integers are
# two's complement, which is what makes the serial-number arithmetic in
# radio/serial.hpp well-defined rather than merely working. Both the Android
# NDK's clang and Xcode's clang support C++20 fully, so there is no portability
# cost.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  # RelWithDebInfo, not Debug, is the honest default here: every latency number
  # this project produces must come from optimised code, and a debug build's
  # numbers would be meaningless. Symbols stay on because profiling is the
  # point.
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(RADIO_BUILD_TESTS "Build unit and conformance tests" ON)
option(RADIO_BUILD_TOOLS "Build host CLI tools (radiobench, impair)" ON)
option(RADIO_WITH_OPUS "Build the Opus-dependent audio layer (M1)" OFF)
option(RADIO_SANITIZE "Enable address+undefined sanitizers" OFF)
option(RADIO_SANITIZE_THREAD "Enable thread sanitizer (excludes RADIO_SANITIZE)" OFF)

# Cross-compiled targets (Android, iOS) build the core only. They exist to prove
# the core stays portable, so tests and host tools are switched off there.
if(CMAKE_CROSSCOMPILING)
  set(RADIO_BUILD_TESTS OFF)
  set(RADIO_BUILD_TOOLS OFF)
endif()

# One interface target carries the warning policy, so it is impossible for a new
# target to quietly opt out of it.
add_library(radio_warnings INTERFACE)
target_compile_options(radio_warnings INTERFACE
  -Wall
  -Wextra
  -Wpedantic
  -Wshadow
  # -Wconversion earns its place in this codebase specifically. Packet code is
  # full of narrowing between size_t, uint32_t, uint16_t and int, and a silent
  # narrowing there is precisely how a length check gets bypassed. Making it a
  # warning forces every narrowing to be written down as a deliberate cast.
  -Wconversion
  -Wsign-conversion
  -Wold-style-cast
  -Wnon-virtual-dtor
  -Wdouble-promotion
  -Wformat=2
)

add_library(radio_sanitizers INTERFACE)
if(RADIO_SANITIZE AND RADIO_SANITIZE_THREAD)
  message(FATAL_ERROR "RADIO_SANITIZE and RADIO_SANITIZE_THREAD are mutually exclusive")
endif()
if(RADIO_SANITIZE)
  target_compile_options(radio_sanitizers INTERFACE
    -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
  target_link_options(radio_sanitizers INTERFACE -fsanitize=address,undefined)
endif()
if(RADIO_SANITIZE_THREAD)
  target_compile_options(radio_sanitizers INTERFACE
    -fsanitize=thread -fno-omit-frame-pointer)
  target_link_options(radio_sanitizers INTERFACE -fsanitize=thread)
endif()

add_subdirectory(core)

if(RADIO_BUILD_TOOLS)
  add_subdirectory(tools)
endif()

if(RADIO_BUILD_TESTS)
  include(CTest)
  add_subdirectory(core/tests)
endif()

message(STATUS "")
message(STATUS "full-duplex-radio configuration")
message(STATUS "  build type : ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++        : ${CMAKE_CXX_STANDARD}")
message(STATUS "  tests      : ${RADIO_BUILD_TESTS}")
message(STATUS "  tools      : ${RADIO_BUILD_TOOLS}")
message(STATUS "  opus       : ${RADIO_WITH_OPUS}")
message(STATUS "  sanitizers : asan/ubsan=${RADIO_SANITIZE} tsan=${RADIO_SANITIZE_THREAD}")
message(STATUS "")
