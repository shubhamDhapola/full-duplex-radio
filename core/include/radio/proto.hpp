// Wire format for the Full-Duplex Radio protocol, version 1.
//
// Normative reference: protocol/specification.md. Conformance contract:
// protocol/testvectors/. If this file and the spec disagree, the spec wins and
// this file is the bug.
#pragma once

#include <cstddef>
#include <cstdint>

#include "radio/bytes.hpp"

namespace radio::proto {

inline constexpr std::uint8_t kVersion = 1;

// See spec §1.1: sized to survive a 1500-byte path MTU with IPv6 plus one
// layer of tunnelling. Voice never approaches it; it exists to bound receive
// buffers and reject nonsense lengths early.
inline constexpr std::size_t kMaxDatagram = 1200;

inline constexpr std::size_t kPrefixSize = 4;
inline constexpr std::size_t kMediaHeaderSize = 16;
inline constexpr std::size_t kControlHeaderSize = 16;
inline constexpr std::size_t kPongBodySize = 16;

// Largest Opus payload we will ever put in one datagram.
inline constexpr std::size_t kMaxPayload = kMaxDatagram - kMediaHeaderSize;

enum class Type : std::uint8_t {
  Audio = 0x01,
  Ping = 0x02,
  Pong = 0x03,
  // Reserved for M3. Recognised by is_assigned() so that a peer running ahead
  // of us is reported as an unimplemented type rather than a protocol error.
  Join = 0x10,
  JoinAck = 0x11,
  Leave = 0x12,
  PttStart = 0x30,
  PttStop = 0x31,
  Metrics = 0x40,
};

[[nodiscard]] constexpr bool is_assigned(Type t) noexcept {
  switch (t) {
    case Type::Audio:
    case Type::Ping:
    case Type::Pong:
    case Type::Join:
    case Type::JoinAck:
    case Type::Leave:
    case Type::PttStart:
    case Type::PttStop:
    case Type::Metrics:
      return true;
  }
  return false;
}

// Media packets use the 16-byte header of spec §3; everything else uses the
// control header of §4. The two happen to be the same size in v1, which is a
// coincidence and must not be relied on.
[[nodiscard]] constexpr bool is_media(Type t) noexcept {
  return t == Type::Audio;
}

namespace media_flag {
inline constexpr std::uint16_t kTalkspurtStart = 0x0001;
inline constexpr std::uint16_t kTalkspurtEnd = 0x0002;
inline constexpr std::uint16_t kFec = 0x0004;
inline constexpr std::uint16_t kDtx = 0x0008;

// Anything outside this mask is reserved, and spec §7 requires we fail closed
// on it. See the comment on Reject::ReservedFlag for why.
inline constexpr std::uint16_t kAssignedMask = 0x000F;
}  // namespace media_flag

struct MediaHeader {
  std::uint16_t flags = 0;
  std::uint32_t stream_id = 0;
  std::uint32_t sequence = 0;
  std::uint32_t timestamp = 0;

  [[nodiscard]] constexpr bool talkspurt_start() const noexcept {
    return (flags & media_flag::kTalkspurtStart) != 0;
  }
  [[nodiscard]] constexpr bool talkspurt_end() const noexcept {
    return (flags & media_flag::kTalkspurtEnd) != 0;
  }
  [[nodiscard]] constexpr bool fec() const noexcept {
    return (flags & media_flag::kFec) != 0;
  }
  [[nodiscard]] constexpr bool dtx() const noexcept {
    return (flags & media_flag::kDtx) != 0;
  }
};

// The payload is a *view into the caller's receive buffer*, not a copy. Media
// parsing happens once per packet on the hot path, so it does no allocation and
// no memcpy; the cost of that choice is that a MediaPacket must not outlive the
// buffer it was parsed from. Ownership is handled one layer up, by the packet
// pool.
struct MediaPacket {
  MediaHeader header;
  ByteView payload;
};

struct ControlHeader {
  Type type{};
  std::uint16_t flags = 0;
  std::uint32_t request_id = 0;
  // Monotonic microseconds on the *sender's* clock. Meaningless to compare
  // across hosts until clock offset has been estimated — see spec §4.1.
  std::uint64_t send_time_us = 0;
};

struct ControlPacket {
  ControlHeader header;
  ByteView body;
};

// Body of a PONG: the two timestamps the responder owes the prober. `t3` rides
// in the control header's send_time_us, and the prober supplies `t4` locally.
struct PongBody {
  std::uint64_t orig_t1 = 0;
  std::uint64_t recv_t2 = 0;
};

// Why a rejection *class* and not a bool.
//
// Spec §6.1 requires each rejection reason be counted separately, because
// operationally they mean completely different things and point at different
// bugs:
//
//   TooShort      on a LAN, something is truncating datagrams — a bad NIC
//                 offload, a broken proxy, or our own send path.
//   BadVersion    a peer is running a different build. Expected during a
//                 rollout, alarming otherwise.
//   UnknownType   a peer is using a feature we don't implement yet.
//   ReservedFlag  a newer peer is trying to tell us something we can't
//                 understand, and we are correctly refusing to guess.
//   BadLength     the type is known but the length is impossible for it.
//
// Collapsing all of that into "invalid packet: 431" throws away the only
// information that would let you diagnose it. Returning the class costs one
// byte and makes the counter self-explanatory.
enum class Reject : std::uint8_t {
  None = 0,
  TooShort,
  BadVersion,
  UnknownType,
  ReservedFlag,
  BadLength,
};

[[nodiscard]] const char* to_string(Reject r) noexcept;

template <class T>
struct Parsed {
  Reject reject = Reject::None;
  T value{};

  [[nodiscard]] constexpr bool ok() const noexcept {
    return reject == Reject::None;
  }
  [[nodiscard]] constexpr explicit operator bool() const noexcept {
    return ok();
  }

  static constexpr Parsed fail(Reject r) noexcept { return Parsed{r, T{}}; }
  static constexpr Parsed good(T v) noexcept {
    return Parsed{Reject::None, static_cast<T&&>(v)};
  }
};

// Classify a datagram by reading only its 4-byte common prefix.
//
// The receive loop calls this first so it can dispatch — and reject garbage —
// without paying for a full parse. This matters less for CPU than for blast
// radius: a malformed packet is rejected by code that has touched four bytes
// and no stream state.
[[nodiscard]] Parsed<Type> peek_type(ByteView datagram) noexcept;

[[nodiscard]] Parsed<MediaPacket> parse_media(ByteView datagram) noexcept;
[[nodiscard]] Parsed<ControlPacket> parse_control(ByteView datagram) noexcept;
[[nodiscard]] Parsed<PongBody> parse_pong_body(ByteView body) noexcept;

// Encoding writes into a caller-supplied buffer and returns the number of bytes
// written, or 0 if `out` is too small. Nothing here allocates: the send path
// runs at 50 packets/second per stream and, on the device, adjacent to an audio
// callback, so a malloc in it is a latency spike waiting to happen.
[[nodiscard]] std::size_t encode_media(const MediaHeader& header,
                                       ByteView payload, ByteSpan out) noexcept;

[[nodiscard]] std::size_t encode_control(const ControlHeader& header,
                                         ByteView body, ByteSpan out) noexcept;

[[nodiscard]] std::size_t encode_pong_body(const PongBody& body,
                                           ByteSpan out) noexcept;

}  // namespace radio::proto
