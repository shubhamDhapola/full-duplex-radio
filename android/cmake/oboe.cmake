# Oboe, fetched at configure time and pinned by content hash.
#
# Same argument as cmake/opus.cmake one layer down: a tag can be moved, a hash
# cannot, and this library ends up inside a shipped application. A dependency
# changing underneath us should be impossible rather than unlikely.
#
# Oboe is Apache 2.0 and is not vendored into this repository; see NOTICE.
#
# WHY OBOE RATHER THAN AAUDIO DIRECTLY
#
# AAudio is the low-latency path and is what we want, but it only exists from
# API 26 and has a handful of device-specific bugs that Oboe works around by
# name. Oboe is a thin header-and-source shim over AAudio that falls back to
# OpenSL ES below 26 and on devices where AAudio misbehaves. It is about 6000
# lines, builds from source in seconds, and adds no runtime dependency.
#
# The fallback is worth being explicit about: if Oboe hands back an OpenSL ES
# stream, latency figures from that device are not comparable with anything
# else, so `usesAAudio` is reported in the diagnostics rather than assumed.
include(FetchContent)

set(RADIO_OBOE_VERSION 1.9.3)

FetchContent_Declare(oboe
  URL      https://github.com/google/oboe/archive/refs/tags/${RADIO_OBOE_VERSION}.tar.gz
  URL_HASH SHA256=9d2486b74bd396d9d9112625077d5eb656fd6942392dc25ebf222b184ff4eb61
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  EXCLUDE_FROM_ALL
)

# Oboe 1.9.3 declares `cmake_minimum_required(VERSION 3.4.1)`, and CMake 4
# refuses anything below 3.5. This is CMake's own documented escape hatch for
# exactly that situation. Scoped to the fetch and unset immediately after, so it
# cannot quietly relax policy for our code too.
#
# Remove this the day Oboe raises its own minimum.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
FetchContent_MakeAvailable(oboe)
unset(CMAKE_POLICY_VERSION_MINIMUM)

# Oboe's headers are not held to this project's warning policy -- they are not
# ours to fix, and -Wold-style-cast against a C-adjacent audio API would bury
# our own warnings. Marking them system-included keeps the signal.
get_target_property(RADIO_OBOE_INCLUDES oboe INTERFACE_INCLUDE_DIRECTORIES)
set_target_properties(oboe PROPERTIES
  INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${RADIO_OBOE_INCLUDES}")
