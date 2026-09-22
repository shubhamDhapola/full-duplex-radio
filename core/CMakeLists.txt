# The core is one static library with no platform dependencies beyond POSIX
# sockets. Everything that knows about Oboe, AVAudioEngine, JNI, permissions or
# UI lives outside it. That boundary is what lets the host benchmarks measure
# the same code that runs on the phone.

add_library(radio_core STATIC
  src/proto/proto.cpp
  src/time/clock.cpp
  src/time/clock_sync.cpp
  src/metrics/seq_tracker.cpp
  src/metrics/jitter_estimator.cpp
  src/metrics/histogram.cpp
  src/metrics/trace.cpp
  src/audio/wav.cpp
  src/audio/reorder_queue.cpp
  src/net/endpoint.cpp
  src/net/udp_socket.cpp
  src/net/pacer.cpp
  src/net/prober.cpp
  src/sim/impairment.cpp
)

if(RADIO_WITH_OPUS)
  # The jitter buffer owns a Decoder, so it belongs on the Opus side of the
  # fence with it. The reorder queue underneath deliberately does not, which is
  # what lets its correctness be tested on a toolchain with no Opus at all.
  target_sources(radio_core PRIVATE
    src/audio/opus_codec.cpp
    src/audio/jitter_buffer.cpp
    include/radio/opus_codec.hpp
    include/radio/jitter_buffer.hpp
  )
  target_link_libraries(radio_core PUBLIC opus)
endif()

target_include_directories(radio_core PUBLIC include)
target_link_libraries(radio_core PRIVATE radio_warnings PUBLIC radio_sanitizers)

# C++20 as a usage requirement, not just a directory-scoped variable.
#
# The top-level CMAKE_CXX_STANDARD only reaches this directory and below, so a
# consumer that adds this repository with add_subdirectory and defines its own
# targets in the parent scope gets the compiler default instead -- and the
# first symptom is `no template named 'span' in namespace 'std'` from
# bytes.hpp, a hundred lines into an unrelated header. The Android JNI library
# hit exactly that; the Swift-facing library in M9 would have hit it next.
#
# Stating it on the target means anything that links radio_core is compiled the
# way radio_core's headers require, wherever it is defined.
target_compile_features(radio_core PUBLIC cxx_std_20)

# Header-only pieces are still listed so IDEs and `cmake --build --target help`
# see them, and so a syntax error in one fails the build rather than waiting for
# a consumer to include it.
target_sources(radio_core PRIVATE
  include/radio/bytes.hpp
  include/radio/endpoint.hpp
  include/radio/clock.hpp
  include/radio/clock_sync.hpp
  include/radio/histogram.hpp
  include/radio/impairment.hpp
  include/radio/jitter_estimator.hpp
  include/radio/pacer.hpp
  include/radio/prober.hpp
  include/radio/proto.hpp
  include/radio/reorder_queue.hpp
  include/radio/seq_tracker.hpp
  include/radio/serial.hpp
  include/radio/spsc_ring.hpp
  include/radio/trace.hpp
  include/radio/udp_socket.hpp
  include/radio/wav.hpp
)

set_target_properties(radio_core PROPERTIES
  POSITION_INDEPENDENT_CODE ON  # required once Android links this into a .so
)

# Linkage check for the Opus dependency, built only when it is enabled. Kept
# outside core/tests so it does not depend on Catch2 and so it can be
# cross-compiled as a bare executable.
if(RADIO_WITH_OPUS)
  add_executable(opus_link_check tests/opus_link_check.cpp)
  target_link_libraries(opus_link_check PRIVATE opus radio_warnings)
endif()
