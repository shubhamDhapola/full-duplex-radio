include(FetchContent)

# Pinned to a tag rather than a branch: a test framework that changes underneath
# you turns "the tests broke" into an ambiguous signal, which defeats the point
# of having them.
FetchContent_Declare(Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG        v3.7.1
  GIT_SHALLOW    TRUE
  EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(Catch2)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)

add_executable(radio_tests
  test_bytes.cpp
  test_serial.cpp
  test_proto.cpp
  test_seq_tracker.cpp
  test_jitter_estimator.cpp
  test_histogram.cpp
  test_endpoint.cpp
  test_udp_socket.cpp
  test_pacer.cpp
)

target_link_libraries(radio_tests
  PRIVATE radio_core radio_warnings Catch2::Catch2WithMain
)

catch_discover_tests(radio_tests)
