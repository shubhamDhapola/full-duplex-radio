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

# JSON, for the conformance test only.
#
# The vectors are JSON because they have to be consumed by Kotlin and Swift too,
# and both have first-class JSON support built in. C++ does not, so the test
# target -- and only the test target -- takes this dependency. radio_core stays
# dependency-free.
FetchContent_Declare(nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        v3.11.3
  GIT_SHALLOW    TRUE
  EXCLUDE_FROM_ALL
)
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)

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
  test_clock_sync.cpp
  test_trace.cpp
  test_impairment.cpp
  test_conformance.cpp
  test_parser_stress.cpp
  test_spsc_ring.cpp
)

target_link_libraries(radio_tests
  PRIVATE radio_core radio_warnings Catch2::Catch2WithMain nlohmann_json::nlohmann_json
)

# The conformance test reads the vector files from the source tree rather than a
# copy, so regenerating them takes effect without a rebuild step.
target_compile_definitions(radio_tests
  PRIVATE RADIO_TESTVECTORS_DIR="${CMAKE_SOURCE_DIR}/protocol/testvectors"
)

if(RADIO_WITH_OPUS)
  target_sources(radio_tests PRIVATE test_opus_codec.cpp)
endif()

catch_discover_tests(radio_tests)

# Optional coverage-guided fuzz target.
#
# AppleClang does not ship libFuzzer, so this is probed rather than assumed:
# a hard requirement would make the default build fail on a stock macOS
# toolchain. The always-available equivalent is test_parser_stress.cpp.
if(RADIO_BUILD_FUZZERS)
  include(CheckCXXSourceCompiles)
  set(CMAKE_REQUIRED_FLAGS "-fsanitize=fuzzer")
  check_cxx_source_compiles([[
    #include <cstddef>
    #include <cstdint>
    extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t*, std::size_t) {
      return 0;
    }
  ]] RADIO_HAVE_LIBFUZZER)
  unset(CMAKE_REQUIRED_FLAGS)

  if(RADIO_HAVE_LIBFUZZER)
    add_executable(fuzz_proto fuzz_proto.cpp)
    target_link_libraries(fuzz_proto PRIVATE radio_core radio_warnings)
    target_compile_options(fuzz_proto PRIVATE -fsanitize=fuzzer)
    target_link_options(fuzz_proto PRIVATE -fsanitize=fuzzer)
  else()
    message(WARNING
      "RADIO_BUILD_FUZZERS=ON but this compiler does not ship libFuzzer "
      "(AppleClang does not). Install Homebrew LLVM and point "
      "CMAKE_CXX_COMPILER at it, or rely on test_parser_stress.cpp which runs "
      "in ctest on every toolchain.")
  endif()
endif()
