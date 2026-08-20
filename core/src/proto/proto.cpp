#include "radio/proto.hpp"

#include <cassert>
#include <cstring>

namespace radio::proto {
namespace {

// Offsets, named so the parse code reads like the spec tables rather than like
// arithmetic. All three headers share bytes 0..3 (spec §2).
constexpr std::size_t kOffVersion = 0;
constexpr std::size_t kOffType = 1;
constexpr std::size_t kOffFlags = 2;

constexpr std::size_t kOffMediaStreamId = 4;
constexpr std::size_t kOffMediaSequence = 8;
constexpr std::size_t kOffMediaTimestamp = 12;

constexpr std::size_t kOffCtlRequestId = 4;
constexpr std::size_t kOffCtlSendTime = 8;

constexpr std::size_t kOffPongOrigT1 = 0;
constexpr std::size_t kOffPongRecvT2 = 8;

// Steps 1-3 of the validation order in spec §6.1, shared by every parse entry
// point. Flags are deliberately *not* checked here: they are type-specific, so
// only the type-aware parser knows which bits are legal.
struct Prefix {
  Reject reject = Reject::None;
  Type type{};
};

[[nodiscard]] Prefix validate_prefix(ByteView d) noexcept {
  if (d.size() < kPrefixSize) return {Reject::TooShort, {}};
  if (std::to_integer<std::uint8_t>(d[kOffVersion]) != kVersion) {
    return {Reject::BadVersion, {}};
  }
  const auto type =
      static_cast<Type>(std::to_integer<std::uint8_t>(d[kOffType]));
  if (!is_assigned(type)) return {Reject::UnknownType, {}};
  return {Reject::None, type};
}

}  // namespace

const char* to_string(Reject r) noexcept {
  switch (r) {
    case Reject::None:
      return "none";
    case Reject::TooShort:
      return "too_short";
    case Reject::BadVersion:
      return "bad_version";
    case Reject::UnknownType:
      return "unknown_type";
    case Reject::ReservedFlag:
      return "reserved_flag";
    case Reject::BadLength:
      return "bad_length";
  }
  return "invalid";
}

Parsed<Type> peek_type(ByteView datagram) noexcept {
  const auto prefix = validate_prefix(datagram);
  if (prefix.reject != Reject::None) return Parsed<Type>::fail(prefix.reject);
  return Parsed<Type>::good(prefix.type);
}

Parsed<MediaPacket> parse_media(ByteView d) noexcept {
  const auto prefix = validate_prefix(d);
  if (prefix.reject != Reject::None) {
    return Parsed<MediaPacket>::fail(prefix.reject);
  }
  // Called with an assigned-but-not-media type. From this function's point of
  // view that is a type it cannot parse, which is the same outcome as an
  // unassigned one. Keeping parse_media total like this is what lets the fuzzer
  // and the test vectors point arbitrary bytes at it.
  if (!is_media(prefix.type)) {
    return Parsed<MediaPacket>::fail(Reject::UnknownType);
  }

  const std::uint16_t flags = load_be16(d.data() + kOffFlags);
  if ((flags & ~media_flag::kAssignedMask) != 0) {
    return Parsed<MediaPacket>::fail(Reject::ReservedFlag);
  }

  // A media packet with no payload carries no audio and no meaning, so an
  // exactly-header-sized datagram is malformed rather than an empty frame.
  if (d.size() <= kMediaHeaderSize || d.size() > kMaxDatagram) {
    return Parsed<MediaPacket>::fail(Reject::BadLength);
  }

  MediaPacket pkt;
  pkt.header.flags = flags;
  pkt.header.stream_id = load_be32(d.data() + kOffMediaStreamId);
  pkt.header.sequence = load_be32(d.data() + kOffMediaSequence);
  pkt.header.timestamp = load_be32(d.data() + kOffMediaTimestamp);
  pkt.payload = d.subspan(kMediaHeaderSize);
  return Parsed<MediaPacket>::good(pkt);
}

Parsed<ControlPacket> parse_control(ByteView d) noexcept {
  const auto prefix = validate_prefix(d);
  if (prefix.reject != Reject::None) {
    return Parsed<ControlPacket>::fail(prefix.reject);
  }
  if (is_media(prefix.type)) {
    return Parsed<ControlPacket>::fail(Reject::UnknownType);
  }

  // Control flags are entirely reserved in v1, so any bit set means a peer is
  // signalling something this build cannot interpret.
  if (load_be16(d.data() + kOffFlags) != 0) {
    return Parsed<ControlPacket>::fail(Reject::ReservedFlag);
  }

  if (d.size() < kControlHeaderSize || d.size() > kMaxDatagram) {
    return Parsed<ControlPacket>::fail(Reject::BadLength);
  }

  ControlPacket pkt;
  pkt.header.type = prefix.type;
  pkt.header.flags = 0;
  pkt.header.request_id = load_be32(d.data() + kOffCtlRequestId);
  pkt.header.send_time_us = load_be64(d.data() + kOffCtlSendTime);
  pkt.body = d.subspan(kControlHeaderSize);
  return Parsed<ControlPacket>::good(pkt);
}

Parsed<PongBody> parse_pong_body(ByteView body) noexcept {
  // Exact length, not a minimum. Accepting trailing bytes would be the
  // fail-open choice, and spec §7 commits to failing closed everywhere: a v2
  // that appends a field to this body bumps the version rather than relying on
  // v1 receivers to ignore what they don't know.
  if (body.size() != kPongBodySize) {
    return Parsed<PongBody>::fail(Reject::BadLength);
  }
  PongBody out;
  out.orig_t1 = load_be64(body.data() + kOffPongOrigT1);
  out.recv_t2 = load_be64(body.data() + kOffPongRecvT2);
  return Parsed<PongBody>::good(out);
}

std::size_t encode_media(const MediaHeader& header, ByteView payload,
                         ByteSpan out) noexcept {
  // Setting a reserved bit would produce a packet every conforming receiver
  // drops. That is a bug in the caller, not a runtime condition, so it trips in
  // debug rather than being silently masked here.
  assert((header.flags & ~media_flag::kAssignedMask) == 0 &&
         "reserved media flag bits must be zero");

  const std::size_t total = kMediaHeaderSize + payload.size();
  if (payload.empty() || total > out.size() || total > kMaxDatagram) return 0;

  std::byte* p = out.data();
  p[kOffVersion] = static_cast<std::byte>(kVersion);
  p[kOffType] = static_cast<std::byte>(Type::Audio);
  store_be16(p + kOffFlags, header.flags);
  store_be32(p + kOffMediaStreamId, header.stream_id);
  store_be32(p + kOffMediaSequence, header.sequence);
  store_be32(p + kOffMediaTimestamp, header.timestamp);
  std::memcpy(p + kMediaHeaderSize, payload.data(), payload.size());
  return total;
}

std::size_t encode_control(const ControlHeader& header, ByteView body,
                           ByteSpan out) noexcept {
  assert(is_assigned(header.type) && !is_media(header.type) &&
         "encode_control requires an assigned control type");
  assert(header.flags == 0 && "control flags are reserved in v1");

  const std::size_t total = kControlHeaderSize + body.size();
  if (total > out.size() || total > kMaxDatagram) return 0;

  std::byte* p = out.data();
  p[kOffVersion] = static_cast<std::byte>(kVersion);
  p[kOffType] = static_cast<std::byte>(header.type);
  store_be16(p + kOffFlags, header.flags);
  store_be32(p + kOffCtlRequestId, header.request_id);
  store_be64(p + kOffCtlSendTime, header.send_time_us);
  if (!body.empty()) {
    std::memcpy(p + kControlHeaderSize, body.data(), body.size());
  }
  return total;
}

std::size_t encode_pong_body(const PongBody& body, ByteSpan out) noexcept {
  if (out.size() < kPongBodySize) return 0;
  store_be64(out.data() + kOffPongOrigT1, body.orig_t1);
  store_be64(out.data() + kOffPongRecvT2, body.recv_t2);
  return kPongBodySize;
}

}  // namespace radio::proto
