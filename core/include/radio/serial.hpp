// Serial-number arithmetic modulo 2^32, in the sense of RFC 1982.
#pragma once

#include <cstdint>

namespace radio::serial {

// Sequence numbers and timestamps in this protocol start at a random 32-bit
// value and wrap. Comparing them with `<` is therefore wrong, and wrong in a
// way that is invisible for hours and then destroys a call:
//
//     a = 0xFFFFFFF0, b = 0x00000005     // b was sent after a
//     a < b   ->  false                  // ...but plain comparison disagrees
//
// The fix is to never compare the values, only their *difference*, and to read
// that difference as signed. Subtraction on unsigned integers is defined to
// wrap, so `b - a` is the true distance modulo 2^32; reinterpreting it as
// int32_t splits the space into "b is ahead of a by less than 2^31" (positive)
// and "b is behind a" (negative).
//
//     b - a = 0x00000015  ->  +21  ->  b follows a. Correct.
//
// TCP compares sequence numbers this way, and RTP uses the same idea for its
// extended sequence space. The assumption it rests on is that the two values
// are genuinely within 2^31 of each other. For a 50 packet/second voice stream
// that horizon is about 1.3 years, so it is safely true here — but it is the
// one way this can break, so it is worth stating rather than discovering.
//
// Note this relies on the uint32 -> int32 conversion for out-of-range values.
// That was implementation-defined before C++20; C++20 mandates two's
// complement and makes it well-defined, which is part of why this project
// targets C++20.

// Signed distance from `from` to `to`: positive if `to` is ahead.
[[nodiscard]] constexpr std::int32_t distance(std::uint32_t from,
                                             std::uint32_t to) noexcept {
  return static_cast<std::int32_t>(to - from);
}

// True if `a` strictly precedes `b`.
[[nodiscard]] constexpr bool precedes(std::uint32_t a, std::uint32_t b) noexcept {
  return distance(a, b) > 0;
}

// True if `a` precedes or equals `b`.
[[nodiscard]] constexpr bool precedes_or_equal(std::uint32_t a,
                                               std::uint32_t b) noexcept {
  return distance(a, b) >= 0;
}

// The later of two serial numbers. Ties return `a`.
[[nodiscard]] constexpr std::uint32_t max(std::uint32_t a,
                                          std::uint32_t b) noexcept {
  return precedes(a, b) ? b : a;
}

}  // namespace radio::serial
