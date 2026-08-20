#include "radio/bytes.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

using namespace radio;

namespace {
constexpr std::byte B(unsigned v) { return static_cast<std::byte>(v); }
}  // namespace

TEST_CASE("big-endian loads read the wire order, not the host order") {
  constexpr std::array<std::byte, 8> buf{B(0x01), B(0x02), B(0x03), B(0x04),
                                         B(0x05), B(0x06), B(0x07), B(0x08)};

  CHECK(load_be16(buf.data()) == 0x0102u);
  CHECK(load_be32(buf.data()) == 0x01020304u);
  CHECK(load_be64(buf.data()) == 0x0102030405060708ull);

  // Values with the high bit set are where a sign-extension bug would show up.
  constexpr std::array<std::byte, 8> high{B(0xFF), B(0xFE), B(0xFD), B(0xFC),
                                          B(0xFB), B(0xFA), B(0xF9), B(0xF8)};
  CHECK(load_be16(high.data()) == 0xFFFEu);
  CHECK(load_be32(high.data()) == 0xFFFEFDFCu);
  CHECK(load_be64(high.data()) == 0xFFFEFDFCFBFAF9F8ull);
}

TEST_CASE("loads and stores are correct at every misalignment") {
  // This is the test that would have caught the reinterpret_cast version. A
  // real UDP payload puts our 32-bit fields wherever the header layout says,
  // not wherever the CPU would prefer, so every offset has to work. Run under
  // -fsanitize=undefined to make a misaligned access fail loudly rather than
  // happening to work on x86.
  std::array<std::byte, 16> buf{};

  for (std::size_t offset = 0; offset < 8; ++offset) {
    store_be16(buf.data() + offset, 0xBEEFu);
    CHECK(load_be16(buf.data() + offset) == 0xBEEFu);

    store_be32(buf.data() + offset, 0xDEADBEEFu);
    CHECK(load_be32(buf.data() + offset) == 0xDEADBEEFu);

    store_be64(buf.data() + offset, 0x0123456789ABCDEFull);
    CHECK(load_be64(buf.data() + offset) == 0x0123456789ABCDEFull);
  }
}

TEST_CASE("stores write the expected bytes and touch nothing else") {
  std::array<std::byte, 6> buf{};
  buf.fill(B(0xAA));

  store_be32(buf.data() + 1, 0x01020304u);
  CHECK(buf[0] == B(0xAA));  // untouched before
  CHECK(buf[1] == B(0x01));
  CHECK(buf[2] == B(0x02));
  CHECK(buf[3] == B(0x03));
  CHECK(buf[4] == B(0x04));
  CHECK(buf[5] == B(0xAA));  // untouched after
}

TEST_CASE("loads are usable in constant expressions") {
  // Not decoration: constexpr is what lets the test vectors and the protocol
  // constants be checked at compile time, and it also proves the
  // implementation contains no UB, since a constant expression may not.
  static constexpr std::array<std::byte, 4> buf{B(0xCA), B(0xFE), B(0xBA),
                                                B(0xBE)};
  static_assert(load_be16(buf.data()) == 0xCAFEu);
  static_assert(load_be32(buf.data()) == 0xCAFEBABEu);
  SUCCEED();
}
