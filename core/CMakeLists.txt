# The core is one static library with no platform dependencies beyond POSIX
# sockets. Everything that knows about Oboe, AVAudioEngine, JNI, permissions or
# UI lives outside it. That boundary is what lets the host benchmarks measure
# the same code that runs on the phone.

add_library(radio_core STATIC
  src/proto/proto.cpp
  src/time/clock.cpp
  src/metrics/seq_tracker.cpp
)

target_include_directories(radio_core PUBLIC include)
target_link_libraries(radio_core PRIVATE radio_warnings PUBLIC radio_sanitizers)

# Header-only pieces are still listed so IDEs and `cmake --build --target help`
# see them, and so a syntax error in one fails the build rather than waiting for
# a consumer to include it.
target_sources(radio_core PRIVATE
  include/radio/bytes.hpp
  include/radio/clock.hpp
  include/radio/proto.hpp
  include/radio/seq_tracker.hpp
  include/radio/serial.hpp
)

set_target_properties(radio_core PROPERTIES
  POSITION_INDEPENDENT_CODE ON  # required once Android links this into a .so
)
