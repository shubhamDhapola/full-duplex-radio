// Big-endian load/store over unaligned byte buffers.
//
// This is the lowest layer in the project and the one most likely to be written
// wrongly. Everything else depends on it being both correct and free.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace radio {

using ByteView = std::span<const std::byte>;
using ByteSpan = std::span<std::byte>;

namespace detail {
constexpr std::uint32_t u8(std::byte b) noexcept {
  return static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(b));
}
}  // namespace detail

// The obvious implementation of these functions is a cast plus a byteswap:
//
//     return ntohl(*reinterpret_cast<const std::uint32_t*>(p));
//
// That line has three independent bugs in it, and packet-parsing code is
// exactly where all three go off:
//
//   1. Alignment. A UDP datagram lands wherever the receive buffer put it, and
//      our fields sit at fixed offsets from the start of the packet, not at
//      addresses the CPU chose. Reading a uint32_t through a pointer that is
//      not 4-byte aligned is undefined behaviour, and on some ARM
//      configurations it faults or silently returns rotated bytes rather than
//      trapping. x86 tolerates it, which is worse: the bug ships and then
//      appears only on the phone.
//
//   2. Strict aliasing. Even when the address happens to be aligned, accessing
//      a buffer of `std::byte` through a `std::uint32_t` lvalue is UB. This is
//      not pedantry — at -O2 both clang and gcc will reorder or elide such
//      accesses, so the failure shows up as "works in debug, broken in
//      release", which is the single most expensive class of bug to chase.
//
//   3. Endianness. `ntohl` is a no-op on a big-endian host, so a cast bakes in
//      an assumption that every test on your laptop confirms and the wire
//      quietly violates.
//
// Assembling the value from individual bytes has none of those problems,
// because a `std::byte` load is always aligned and never aliases. It also
// costs nothing: clang and gcc both recognise this exact pattern and emit a
// single unaligned load plus one `rev`/`bswap` instruction. Correctness here is
// free, so there is no tradeoff to think about.

[[nodiscard]] constexpr std::uint16_t load_be16(const std::byte* p) noexcept {
  using detail::u8;
  return static_cast<std::uint16_t>((u8(p[0]) << 8) | u8(p[1]));
}

[[nodiscard]] constexpr std::uint32_t load_be32(const std::byte* p) noexcept {
  using detail::u8;
  return (u8(p[0]) << 24) | (u8(p[1]) << 16) | (u8(p[2]) << 8) | u8(p[3]);
}

[[nodiscard]] constexpr std::uint64_t load_be64(const std::byte* p) noexcept {
  return (static_cast<std::uint64_t>(load_be32(p)) << 32) | load_be32(p + 4);
}

constexpr void store_be16(std::byte* p, std::uint16_t v) noexcept {
  p[0] = static_cast<std::byte>((v >> 8) & 0xFF);
  p[1] = static_cast<std::byte>(v & 0xFF);
}

constexpr void store_be32(std::byte* p, std::uint32_t v) noexcept {
  p[0] = static_cast<std::byte>((v >> 24) & 0xFF);
  p[1] = static_cast<std::byte>((v >> 16) & 0xFF);
  p[2] = static_cast<std::byte>((v >> 8) & 0xFF);
  p[3] = static_cast<std::byte>(v & 0xFF);
}

constexpr void store_be64(std::byte* p, std::uint64_t v) noexcept {
  store_be32(p, static_cast<std::uint32_t>(v >> 32));
  store_be32(p + 4, static_cast<std::uint32_t>(v & 0xFFFFFFFFu));
}

}  // namespace radio
