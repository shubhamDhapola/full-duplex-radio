# libopus, fetched at configure time and pinned by content hash.
#
# Pinned to an exact tarball and SHA256 rather than a git tag. A tag can be
# moved; a hash cannot. Since this library ends up inside a shipped application,
# "the dependency changed underneath us" should be impossible rather than
# unlikely, and a hash mismatch fails the configure step loudly.
#
# Opus is BSD 3-Clause and is not vendored into this repository; see NOTICE.

include(FetchContent)

set(RADIO_OPUS_VERSION 1.5.2)

FetchContent_Declare(opus
  URL      https://downloads.xiph.org/releases/opus/opus-${RADIO_OPUS_VERSION}.tar.gz
  URL_HASH SHA256=65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  EXCLUDE_FROM_ALL
)

# Static, and nothing but the library itself. The demo programs and test suite
# would add minutes to a cold build and are not our code to validate.
set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS       OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING        OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE   OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)

# Opus 1.5 added neural-network features: DRED (deep redundancy) and OSCE
# (speech enhancement), plus a DNN-based packet-loss concealer. They are
# deliberately off for now.
#
# Deep PLC in particular is directly relevant to this project and worth
# revisiting once the classical concealment path is measured -- but it brings a
# model blob and a materially different CPU profile, and enabling it before the
# baseline exists would make the loss-recovery comparison in M1 impossible to
# interpret. Establish the baseline first, then measure what it buys.
set(OPUS_DRED      OFF CACHE BOOL "" FORCE)
set(OPUS_OSCE      OFF CACHE BOOL "" FORCE)
set(OPUS_DEEP_PLC  OFF CACHE BOOL "" FORCE)

# Opus turns its own test suite on if EITHER its option or the generic
# BUILD_TESTING is set:
#
#     if(OPUS_BUILD_TESTING OR BUILD_TESTING)
#       set(OPUS_BUILD_TESTING ON)
#
# and this project calls include(CTest), which sets BUILD_TESTING=ON in the
# cache. So OPUS_BUILD_TESTING=OFF above is not sufficient on its own.
#
# Worse, it was order-dependent: on a first configure the cache has no
# BUILD_TESTING yet and Opus's tests stayed off, but on every configure after
# that the cached ON turned them on. A build that behaves differently the second
# time it is configured is a trap, so shadow the variable for the duration of
# the subdirectory rather than relying on ordering.
#
# Upstream's tests are not ours to validate, they add minutes to a cold build,
# and several need test vectors that are not fetched here. Our own coverage is
# opus_link_check plus core/tests/test_opus_codec.cpp.
# A normal variable shadows the cache entry for the duration of the
# subdirectory, then unset() removes the shadow so the cache value shows through
# again. Restoring it with set() instead would leave a normal variable behind
# that shadows the cache permanently -- which breaks include(CTest), because it
# then sees BUILD_TESTING already defined and never creates the cache option its
# test registration depends on. The first attempt at this fix did exactly that
# and silently reduced our own suite to zero registered tests.
set(BUILD_TESTING OFF)

FetchContent_MakeAvailable(opus)

unset(BUILD_TESTING)

# Opus builds with its own flags, but its headers are reached through our
# targets, so mark them SYSTEM. Without this our -Wall -Wextra -Wconversion set
# would report warnings from third-party headers we are not going to fix, and a
# warning we always ignore trains us to ignore warnings.
if(TARGET opus)
  get_target_property(_opus_includes opus INTERFACE_INCLUDE_DIRECTORIES)
  if(_opus_includes)
    set_target_properties(opus PROPERTIES
      INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_opus_includes}")
  endif()
endif()

message(STATUS "  opus       : ${RADIO_OPUS_VERSION} (static, DNN features off)")
