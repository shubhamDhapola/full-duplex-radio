// libFuzzer entry point for the packet parsers.
//
// Not built by default, and not buildable at all with AppleClang, which does not
// ship libFuzzer. The portable equivalent that always runs in ctest is
// test_parser_stress.cpp; this target exists for coverage-guided runs on a
// toolchain that supports it.
//
//   brew install llvm
//   cmake -B build-fuzz -G Ninja \
//     -DCMAKE_C_COMPILER=/opt/homebrew/opt/llvm/bin/clang \
//     -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++ \
//     -DRADIO_BUILD_FUZZERS=ON -DRADIO_SANITIZE=ON -DCMAKE_BUILD_TYPE=Debug
//   cmake --build build-fuzz
//   ./build-fuzz/core/tests/fuzz_proto -max_total_time=60
//
// Seed the corpus from the conformance vectors for a much faster start:
//   python3 -c "import json,pathlib; \
//     d=json.load(open('protocol/testvectors/vectors.json')); \
//     p=pathlib.Path('corpus'); p.mkdir(exist_ok=True); \
//     [ (p/v['name']).write_bytes(bytes.fromhex(v['hex'])) for v in d['vectors'] ]"
//   ./build-fuzz/core/tests/fuzz_proto corpus/
#include <cstddef>
#include <cstdint>

#include "radio/proto.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
  // reinterpret_cast from the fuzzer's uint8_t* is unavoidable and safe: the
  // bytes are being viewed as bytes.
  const radio::ByteView datagram{reinterpret_cast<const std::byte*>(data), size};

  volatile std::uint64_t sink = 0;

  sink += static_cast<std::uint64_t>(radio::proto::peek_type(datagram).reject);

  const auto media = radio::proto::parse_media(datagram);
  sink += static_cast<std::uint64_t>(media.reject);
  if (media.ok()) {
    // Read the payload so a span that escaped the input buffer faults under
    // ASan rather than passing silently.
    for (const auto byte : media.value.payload) {
      sink += std::to_integer<std::uint8_t>(byte);
    }
  }

  const auto control = radio::proto::parse_control(datagram);
  sink += static_cast<std::uint64_t>(control.reject);
  if (control.ok()) {
    for (const auto byte : control.value.body) {
      sink += std::to_integer<std::uint8_t>(byte);
    }
    const auto pong = radio::proto::parse_pong_body(control.value.body);
    if (pong.ok()) sink += pong.value.orig_t1 ^ pong.value.recv_t2;
  }

  (void)sink;
  return 0;
}
