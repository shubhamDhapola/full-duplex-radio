// Adversarial-input stress for the packet parsers.
//
// WHY THIS EXISTS ALONGSIDE A FUZZ TARGET
//
// Coverage-guided fuzzing (fuzz_proto.cpp) is better at finding paths, but it
// needs a clang that ships libFuzzer -- and AppleClang does not, so on a stock
// macOS toolchain the fuzz target simply cannot be built. A parser that is only
// hardened on machines with Homebrew LLVM installed is not hardened.
//
// So this runs in ctest on every platform, every build, with a fixed seed. It
// is weaker than libFuzzer in general, but note the shape of what it is
// testing: proto.cpp has no loops, no recursion, and no variable-length fields.
// The entire attack surface is a handful of length comparisons and fixed-offset
// loads. Random plus mutation-of-valid over that shape gets close to
// exhaustive, which is not a claim that would hold for, say, an ASN.1 or a
// video container parser.
//
// Run it under sanitizers for it to mean anything:
//   cmake -B build-san -DRADIO_SANITIZE=ON -DCMAKE_BUILD_TYPE=Debug
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "radio/proto.hpp"

using namespace radio;

namespace {

// splitmix64, so the sequence is identical on every platform and a failure is
// reproducible from the seed alone. std::mt19937 plus a distribution would not
// be -- see the note on ImpairmentEngine's hand-rolled transforms.
class Random {
 public:
  explicit Random(std::uint64_t seed) noexcept : state_(seed) {}

  std::uint64_t next() noexcept {
    state_ += 0x9E3779B97F4A7C15ull;
    std::uint64_t z = state_;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }

  std::size_t below(std::size_t bound) noexcept {
    return bound == 0 ? 0 : static_cast<std::size_t>(next() % bound);
  }

  std::byte byte() noexcept { return static_cast<std::byte>(next() & 0xFF); }

 private:
  std::uint64_t state_;
};

// Prevents the optimiser from deleting the parse calls entirely once it proves
// the results are unused -- which would turn this whole file into a no-op.
volatile std::uint64_t g_sink = 0;

// Runs every parser over `datagram` and checks the one invariant that matters
// beyond "did not crash": a successful parse must return a view that lies
// entirely inside the input buffer.
//
// This is worth asserting rather than leaving to the sanitiser. An
// out-of-bounds span is only caught by ASan when something reads it, and
// nothing here has to read it -- so the bug could sit undetected until a caller
// in M1 dereferences the tail. Checking the bounds directly catches it at the
// source.
void exercise(ByteView datagram) {
  const auto* begin = datagram.data();
  const auto* end = begin + datagram.size();

  const auto type = proto::peek_type(datagram);
  g_sink += static_cast<std::uint64_t>(type.reject);

  const auto media = proto::parse_media(datagram);
  g_sink += static_cast<std::uint64_t>(media.reject);
  if (media.ok()) {
    const auto& payload = media.value.payload;
    REQUIRE(payload.data() >= begin);
    REQUIRE(payload.data() + payload.size() <= end);
    REQUIRE_FALSE(payload.empty());
    // Touch every byte, so ASan would fault on a span that escaped the buffer.
    std::uint64_t checksum = 0;
    for (const auto byte : payload) {
      checksum += std::to_integer<std::uint8_t>(byte);
    }
    g_sink += checksum;
  }

  const auto control = proto::parse_control(datagram);
  g_sink += static_cast<std::uint64_t>(control.reject);
  if (control.ok()) {
    const auto& body = control.value.body;
    REQUIRE(body.data() >= begin);
    REQUIRE(body.data() + body.size() <= end);
    std::uint64_t checksum = 0;
    for (const auto byte : body) {
      checksum += std::to_integer<std::uint8_t>(byte);
    }
    g_sink += checksum;

    const auto pong = proto::parse_pong_body(body);
    g_sink += static_cast<std::uint64_t>(pong.reject);
    if (pong.ok()) g_sink += pong.value.orig_t1 + pong.value.recv_t2;
  }

  // Exactly one outcome per parser: a parse either succeeds or names a class.
  // A result with Reject::None and no value would be a silent third state.
  REQUIRE((media.ok() || media.reject != proto::Reject::None));
  REQUIRE((control.ok() || control.reject != proto::Reject::None));
}

std::vector<std::byte> valid_media(Random& random, std::size_t payload_bytes) {
  proto::MediaHeader header;
  header.flags = static_cast<std::uint16_t>(random.next() &
                                            proto::media_flag::kAssignedMask);
  header.stream_id = static_cast<std::uint32_t>(random.next());
  header.sequence = static_cast<std::uint32_t>(random.next());
  header.timestamp = static_cast<std::uint32_t>(random.next());

  std::vector<std::byte> payload(payload_bytes);
  for (auto& byte : payload) byte = random.byte();

  std::vector<std::byte> out(proto::kMediaHeaderSize + payload_bytes);
  const auto written = proto::encode_media(header, payload, out);
  REQUIRE(written == out.size());
  return out;
}

}  // namespace

TEST_CASE("every datagram length from 0 to 64 bytes is handled") {
  // Exhaustive over the lengths where the boundary logic lives. Two passes:
  // all-zero bytes, then a version byte that is actually valid so the length
  // checks are reached rather than short-circuited.
  std::vector<std::byte> buffer(64);

  for (std::size_t length = 0; length <= 64; ++length) {
    exercise(ByteView{buffer.data(), length});
  }

  buffer[0] = static_cast<std::byte>(proto::kVersion);
  buffer[1] = static_cast<std::byte>(proto::Type::Audio);
  for (std::size_t length = 0; length <= 64; ++length) {
    exercise(ByteView{buffer.data(), length});
  }

  buffer[1] = static_cast<std::byte>(proto::Type::Pong);
  for (std::size_t length = 0; length <= 64; ++length) {
    exercise(ByteView{buffer.data(), length});
  }
}

TEST_CASE("all 65536 prefix combinations of version and type are handled") {
  // Exhaustive over the two bytes that decide dispatch, at a length long enough
  // for either header to be complete.
  std::vector<std::byte> buffer(64);
  for (unsigned version = 0; version < 256; ++version) {
    for (unsigned type = 0; type < 256; ++type) {
      buffer[0] = static_cast<std::byte>(version);
      buffer[1] = static_cast<std::byte>(type);
      exercise(ByteView{buffer.data(), buffer.size()});
    }
  }
}

TEST_CASE("all 65536 flag values are handled for a media packet") {
  // Exhaustive over the flags field, which is the fail-closed check.
  Random random(1);
  auto packet = valid_media(random, 8);
  for (unsigned flags = 0; flags < 65536; ++flags) {
    store_be16(packet.data() + 2, static_cast<std::uint16_t>(flags));
    const auto parsed = proto::parse_media(packet);
    // Anything outside the assigned mask must be rejected, and nothing inside
    // it may be.
    if ((flags & ~static_cast<unsigned>(proto::media_flag::kAssignedMask)) !=
        0) {
      REQUIRE(parsed.reject == proto::Reject::ReservedFlag);
    } else {
      REQUIRE(parsed.ok());
    }
  }
}

TEST_CASE("random bytes never break a parser") {
  Random random(0xC0FFEE);
  std::vector<std::byte> buffer(proto::kMaxDatagram + 64);

  for (int iteration = 0; iteration < 120'000; ++iteration) {
    const std::size_t length = random.below(buffer.size() + 1);
    for (std::size_t i = 0; i < length; ++i) buffer[i] = random.byte();
    exercise(ByteView{buffer.data(), length});
  }
  CHECK(g_sink != 0);  // the calls were not optimised away
}

TEST_CASE("mutations of valid packets never break a parser") {
  // The productive case. Purely random bytes are rejected at the version check
  // almost every time, so they barely reach the interesting code. Starting from
  // a valid packet and perturbing it lands on the boundaries.
  Random random(0xBADC0DE);

  for (int iteration = 0; iteration < 120'000; ++iteration) {
    const std::size_t payload_bytes = 1 + random.below(200);
    auto packet = valid_media(random, payload_bytes);

    switch (random.below(6)) {
      case 0: {  // flip a bit anywhere
        const auto index = random.below(packet.size());
        const auto bit = static_cast<unsigned>(random.below(8));
        packet[index] ^= static_cast<std::byte>(1u << bit);
        break;
      }
      case 1: {  // replace a byte
        packet[random.below(packet.size())] = random.byte();
        break;
      }
      case 2: {  // truncate to any shorter length, including zero
        packet.resize(random.below(packet.size() + 1));
        break;
      }
      case 3: {  // extend past the datagram cap
        packet.resize(proto::kMaxDatagram + 1 + random.below(64),
                      random.byte());
        break;
      }
      case 4: {  // corrupt just the header, leaving a plausible payload
        const auto index = random.below(proto::kMediaHeaderSize);
        packet[index] = random.byte();
        break;
      }
      case 5: {  // retarget at the control parser
        packet[1] = static_cast<std::byte>(
            random.below(2) != 0 ? proto::Type::Pong : proto::Type::Ping);
        break;
      }
      default:
        break;
    }

    exercise(ByteView{packet.data(), packet.size()});
  }
}

TEST_CASE("encoding never overruns a buffer of any size") {
  // The mirror of the parse tests. encode_* must refuse rather than write past
  // the end, for every combination of payload size and output capacity around
  // the boundary.
  Random random(7);
  std::vector<std::byte> payload(64);
  for (auto& byte : payload) byte = random.byte();

  proto::MediaHeader header;
  header.stream_id = 1;

  for (std::size_t payload_bytes = 0; payload_bytes <= 64; ++payload_bytes) {
    for (std::size_t capacity = 0; capacity <= 96; ++capacity) {
      // A canary after the writable region: if encode_media wrote past
      // `capacity` this byte would change.
      std::vector<std::byte> out(capacity + 8, static_cast<std::byte>(0x5A));
      const ByteSpan writable{out.data(), capacity};
      const ByteView slice{payload.data(), payload_bytes};

      const auto written = proto::encode_media(header, slice, writable);
      if (payload_bytes == 0) {
        REQUIRE(written == 0);
      } else if (capacity >= proto::kMediaHeaderSize + payload_bytes) {
        REQUIRE(written == proto::kMediaHeaderSize + payload_bytes);
      } else {
        REQUIRE(written == 0);
      }
      for (std::size_t i = capacity; i < out.size(); ++i) {
        REQUIRE(out[i] == static_cast<std::byte>(0x5A));
      }
    }
  }
}
