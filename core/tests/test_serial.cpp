#include "radio/serial.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

using namespace radio;

TEST_CASE("serial comparison behaves like plain comparison far from the wrap") {
  CHECK(serial::precedes(100, 101));
  CHECK_FALSE(serial::precedes(101, 100));
  CHECK_FALSE(serial::precedes(100, 100));
  CHECK(serial::precedes_or_equal(100, 100));
  CHECK(serial::distance(100, 140) == 40);
  CHECK(serial::distance(140, 100) == -40);
}

TEST_CASE("serial comparison survives the 2^32 wrap") {
  // The case that plain `<` gets wrong, and the reason this header exists.
  constexpr std::uint32_t before = 0xFFFFFFF0u;
  constexpr std::uint32_t after = 0x00000005u;

  CHECK(before > after);  // plain comparison disagrees with reality...
  CHECK(serial::precedes(before, after));  // ...and serial arithmetic doesn't
  CHECK_FALSE(serial::precedes(after, before));
  CHECK(serial::distance(before, after) == 21);
  CHECK(serial::distance(after, before) == -21);
  CHECK(serial::max(before, after) == after);
}

TEST_CASE("serial comparison is exact at the half-range boundary") {
  // The ordering is only defined while the two values are within 2^31 of each
  // other. Right at the boundary the answer flips, and pinning that down is
  // what stops a future "optimisation" from quietly changing the semantics.
  constexpr std::uint32_t base = 1000;

  CHECK(serial::precedes(base, base + 0x7FFFFFFFu));
  CHECK(serial::distance(base, base + 0x7FFFFFFFu) == 0x7FFFFFFF);

  // Exactly 2^31 apart is the ambiguous point: the difference is INT32_MIN, so
  // it reads as negative and the relation reverses. Documented, not accidental.
  CHECK_FALSE(serial::precedes(base, base + 0x80000000u));
  CHECK(serial::distance(base, base + 0x80000000u) == INT32_MIN);
}

TEST_CASE("a full lap of increments never reports going backwards") {
  // A stream advancing one packet at a time must always look forward, including
  // across the wrap. Start just below the wrap so the loop crosses it.
  std::uint32_t seq = 0xFFFFFFFAu;
  for (int i = 0; i < 32; ++i) {
    const std::uint32_t next = seq + 1;
    REQUIRE(serial::precedes(seq, next));
    REQUIRE(serial::distance(seq, next) == 1);
    seq = next;
  }
  CHECK(seq == 0x0000001Au);
}
