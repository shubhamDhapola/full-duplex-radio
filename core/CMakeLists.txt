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
  src/net/endpoint.cpp
  src/net/udp_socket.cpp
  src/net/pacer.cpp
)

target_include_directories(radio_core PUBLIC include)
target_link_libraries(radio_core PRIVATE radio_warnings PUBLIC radio_sanitizers)

# Header-only pieces are still listed so IDEs and `cmake --build --target help`
# see them, and so a syntax error in one fails the build rather than waiting for
# a consumer to include it.
target_sources(radio_core PRIVATE
  include/radio/bytes.hpp
  include/radio/endpoint.hpp
  include/radio/clock.hpp
  include/radio/clock_sync.hpp
  include/radio/histogram.hpp
  include/radio/jitter_estimator.hpp
  include/radio/pacer.hpp
  include/radio/proto.hpp
  include/radio/seq_tracker.hpp
  include/radio/serial.hpp
  include/radio/udp_socket.hpp
)

set_target_properties(radio_core PROPERTIES
  POSITION_INDEPENDENT_CODE ON  # required once Android links this into a .so
)
