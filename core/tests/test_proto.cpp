#include "radio/proto.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

using namespace radio;
using namespace radio::proto;

namespace {

constexpr std::byte B(unsigned v) { return static_cast<std::byte>(v); }

// A minimal valid AUDIO datagram: 16-byte header plus one payload byte.
std::vector<std::byte> media_datagram(std::uint16_t flags = 0,
                                      std::size_t payload_bytes = 4) {
  std::vector<std::byte> d(kMediaHeaderSize + payload_bytes, B(0));
  d[0] = B(kVersion);
  d[1] = static_cast<std::byte>(Type::Audio);
  store_be16(d.data() + 2, flags);
  store_be32(d.data() + 4, 0x11223344u);   // stream_id
  store_be32(d.data() + 8, 0x55667788u);   // sequence
  store_be32(d.data() + 12, 0x99AABBCCu);  // timestamp
  for (std::size_t i = 0; i < payload_bytes; ++i) {
    d[kMediaHeaderSize + i] = static_cast<std::byte>(0xE0u + i);
  }
  return d;
}

std::vector<std::byte> control_datagram(Type type, std::size_t body_bytes = 0) {
  std::vector<std::byte> d(kControlHeaderSize + body_bytes, B(0));
  d[0] = B(kVersion);
  d[1] = static_cast<std::byte>(type);
  store_be16(d.data() + 2, 0);
  store_be32(d.data() + 4, 0xABCDEF01u);            // request_id
  store_be64(d.data() + 8, 0x0102030405060708ull);  // send_time_us
  return d;
}

}  // namespace

TEST_CASE("media packets round-trip through encode and parse") {
  MediaHeader hdr;
  hdr.flags = media_flag::kTalkspurtStart | media_flag::kFec;
  hdr.stream_id = 0xDEADBEEFu;
  hdr.sequence = 0xFFFFFFF0u;  // near the wrap, since that must survive too
  hdr.timestamp = 0x000003C0u;

  const std::array<std::byte, 5> payload{B(1), B(2), B(3), B(4), B(5)};
  std::array<std::byte, kMaxDatagram> out{};

  const std::size_t n = encode_media(hdr, payload, out);
  REQUIRE(n == kMediaHeaderSize + payload.size());

  const auto parsed = parse_media(ByteView{out.data(), n});
  REQUIRE(parsed.ok());
  CHECK(parsed.value.header.flags == hdr.flags);
  CHECK(parsed.value.header.stream_id == hdr.stream_id);
  CHECK(parsed.value.header.sequence == hdr.sequence);
  CHECK(parsed.value.header.timestamp == hdr.timestamp);
  CHECK(parsed.value.header.talkspurt_start());
  CHECK(parsed.value.header.fec());
  CHECK_FALSE(parsed.value.header.talkspurt_end());
  CHECK_FALSE(parsed.value.header.dtx());
  REQUIRE(parsed.value.payload.size() == payload.size());
  CHECK(parsed.value.payload[0] == B(1));
  CHECK(parsed.value.payload[4] == B(5));
}

TEST_CASE("a parsed payload aliases the input buffer rather than copying it") {
  // The zero-copy contract from proto.hpp. If this ever starts copying, the hot
  // path silently gains an allocation per packet, so it is worth asserting
  // rather than trusting.
  const auto d = media_datagram();
  const auto parsed = parse_media(d);
  REQUIRE(parsed.ok());
  CHECK(parsed.value.payload.data() == d.data() + kMediaHeaderSize);
}

TEST_CASE("the header layout matches the specification byte for byte") {
  // Guards against a well-meaning refactor reordering fields. The spec's §3
  // table is the source of truth; these offsets are transcribed from it.
  MediaHeader hdr;
  hdr.flags = media_flag::kDtx;
  hdr.stream_id = 0x01020304u;
  hdr.sequence = 0x05060708u;
  hdr.timestamp = 0x090A0B0Cu;

  const std::array<std::byte, 1> payload{B(0x7F)};
  std::array<std::byte, 32> out{};
  const std::size_t n = encode_media(hdr, payload, out);
  REQUIRE(n == 17);

  CHECK(out[0] == B(0x01));  // version
  CHECK(out[1] == B(0x01));  // type = AUDIO
  CHECK(out[2] == B(0x00));  // flags hi
  CHECK(out[3] == B(0x08));  // flags lo = DTX
  CHECK(load_be32(out.data() + 4) == 0x01020304u);
  CHECK(load_be32(out.data() + 8) == 0x05060708u);
  CHECK(load_be32(out.data() + 12) == 0x090A0B0Cu);
  CHECK(out[16] == B(0x7F));
}

TEST_CASE("peek_type classifies from the four-byte prefix alone") {
  std::array<std::byte, 4> prefix{
      B(kVersion), static_cast<std::byte>(Type::Ping), B(0), B(0)};
  const auto t = peek_type(prefix);
  REQUIRE(t.ok());
  CHECK(t.value == Type::Ping);
}

TEST_CASE("undersized datagrams are rejected as too short") {
  const auto d = media_datagram();
  for (std::size_t len = 0; len < kPrefixSize; ++len) {
    CHECK(parse_media(ByteView{d.data(), len}).reject == Reject::TooShort);
    CHECK(peek_type(ByteView{d.data(), len}).reject == Reject::TooShort);
  }
}

TEST_CASE("a wrong version byte is rejected before anything else is read") {
  auto d = media_datagram();
  d[0] = B(2);
  CHECK(parse_media(d).reject == Reject::BadVersion);
}

TEST_CASE("unassigned type codes are rejected") {
  auto d = media_datagram();
  d[1] = B(0x7E);
  CHECK(parse_media(d).reject == Reject::UnknownType);
  CHECK(peek_type(d).reject == Reject::UnknownType);
}

TEST_CASE("parse_media refuses a control type and vice versa") {
  // Each parser validates the whole prefix itself, so either can be pointed at
  // an arbitrary datagram. That is what makes them safe to fuzz.
  CHECK(parse_media(control_datagram(Type::Ping)).reject ==
        Reject::UnknownType);
  CHECK(parse_control(media_datagram()).reject == Reject::UnknownType);
}

TEST_CASE("reserved media flag bits fail closed") {
  // Spec §7: a receiver that ignored an unknown flag would decode a packet it
  // does not understand, and the bug would surface as an audio artefact instead
  // of a counter.
  for (int bit = 4; bit < 16; ++bit) {
    const auto mask = static_cast<std::uint16_t>(1u << bit);
    const auto d = media_datagram(mask);
    CHECK(parse_media(d).reject == Reject::ReservedFlag);
  }
  // ...while every assigned combination is accepted.
  for (std::uint16_t f = 0; f <= media_flag::kAssignedMask; ++f) {
    CHECK(parse_media(media_datagram(f)).ok());
  }
}

TEST_CASE("reserved control flags fail closed") {
  auto d = control_datagram(Type::Ping);
  store_be16(d.data() + 2, 0x0001u);
  CHECK(parse_control(d).reject == Reject::ReservedFlag);
}

TEST_CASE("media length boundaries are enforced exactly") {
  // Exactly the header and no payload: no audio, so malformed rather than
  // "an empty frame".
  const auto d = media_datagram(0, 0);
  REQUIRE(d.size() == kMediaHeaderSize);
  CHECK(parse_media(d).reject == Reject::BadLength);

  // One payload byte is the smallest legal media packet.
  CHECK(parse_media(media_datagram(0, 1)).ok());

  // Largest legal, and one byte past it.
  CHECK(parse_media(media_datagram(0, kMaxPayload)).ok());
  CHECK(parse_media(media_datagram(0, kMaxPayload + 1)).reject ==
        Reject::BadLength);
}

TEST_CASE("validation happens in the order the specification mandates") {
  // A datagram can be wrong in several ways at once. The spec fixes the
  // reported class so that telemetry is comparable between implementations,
  // rather than depending on which check a given port happened to write first.
  auto d = media_datagram(0xFFF0u, 0);  // bad flags AND header-only length
  d[0] = B(9);                          // ...and a bad version
  d[1] = B(0x7E);                       // ...and an unassigned type
  CHECK(parse_media(d).reject == Reject::BadVersion);

  d[0] = B(kVersion);
  CHECK(parse_media(d).reject == Reject::UnknownType);

  d[1] = static_cast<std::byte>(Type::Audio);
  CHECK(parse_media(d).reject == Reject::ReservedFlag);

  store_be16(d.data() + 2, 0);
  CHECK(parse_media(d).reject == Reject::BadLength);
}

TEST_CASE("control packets round-trip, with and without a body") {
  ControlHeader hdr;
  hdr.type = Type::Ping;
  hdr.request_id = 0x0BADF00Du;
  hdr.send_time_us = 0xFFFFFFFFFFFFFFFFull;  // full 64-bit range must survive

  std::array<std::byte, kMaxDatagram> out{};
  const std::size_t n = encode_control(hdr, ByteView{}, out);
  REQUIRE(n == kControlHeaderSize);

  const auto parsed = parse_control(ByteView{out.data(), n});
  REQUIRE(parsed.ok());
  CHECK(parsed.value.header.type == Type::Ping);
  CHECK(parsed.value.header.request_id == hdr.request_id);
  CHECK(parsed.value.header.send_time_us == hdr.send_time_us);
  CHECK(parsed.value.body.empty());
}

TEST_CASE("a PONG carries the two timestamps the prober cannot know") {
  const PongBody body{.orig_t1 = 1'000'000ull, .recv_t2 = 1'000'450ull};

  std::array<std::byte, kPongBodySize> encoded{};
  REQUIRE(encode_pong_body(body, encoded) == kPongBodySize);

  ControlHeader hdr;
  hdr.type = Type::Pong;
  hdr.request_id = 7;
  hdr.send_time_us = 1'000'480ull;  // t3

  std::array<std::byte, kMaxDatagram> out{};
  const std::size_t n = encode_control(hdr, encoded, out);
  REQUIRE(n == kControlHeaderSize + kPongBodySize);

  const auto pkt = parse_control(ByteView{out.data(), n});
  REQUIRE(pkt.ok());
  const auto parsed_body = parse_pong_body(pkt.value.body);
  REQUIRE(parsed_body.ok());
  CHECK(parsed_body.value.orig_t1 == body.orig_t1);
  CHECK(parsed_body.value.recv_t2 == body.recv_t2);

  // The RTT and offset arithmetic of spec §4.1, on known inputs.
  const std::uint64_t t1 = parsed_body.value.orig_t1;
  const std::uint64_t t2 = parsed_body.value.recv_t2;
  const std::uint64_t t3 = pkt.value.header.send_time_us;
  const std::uint64_t t4 = 1'000'600ull;

  const auto rtt =
      static_cast<std::int64_t>(t4 - t1) - static_cast<std::int64_t>(t3 - t2);
  const auto offset =
      (static_cast<std::int64_t>(t2) - static_cast<std::int64_t>(t1) +
       static_cast<std::int64_t>(t3) - static_cast<std::int64_t>(t4)) /
      2;
  CHECK(rtt == 570);     // 600 total, minus 30 spent inside the responder
  CHECK(offset == 165);  // responder's clock runs 165 us ahead
}

TEST_CASE("a PONG body must be exactly the specified length") {
  std::array<std::byte, kPongBodySize + 4> buf{};
  CHECK(parse_pong_body(ByteView{buf.data(), kPongBodySize - 1}).reject ==
        Reject::BadLength);
  CHECK(parse_pong_body(ByteView{buf.data(), kPongBodySize}).ok());
  CHECK(parse_pong_body(ByteView{buf.data(), kPongBodySize + 1}).reject ==
        Reject::BadLength);
}

TEST_CASE("encoding into too small a buffer fails instead of overrunning") {
  MediaHeader hdr;
  const std::array<std::byte, 8> payload{};

  for (std::size_t cap = 0; cap < kMediaHeaderSize + payload.size(); ++cap) {
    std::vector<std::byte> out(cap);
    CHECK(encode_media(hdr, payload, out) == 0);
  }
  std::vector<std::byte> exact(kMediaHeaderSize + payload.size());
  CHECK(encode_media(hdr, payload, exact) == exact.size());
}

TEST_CASE("encoding refuses to build a packet larger than the datagram cap") {
  MediaHeader hdr;
  const std::vector<std::byte> payload(kMaxPayload + 1);
  std::vector<std::byte> out(kMaxDatagram + 64);  // buffer is big enough...
  CHECK(encode_media(hdr, payload, out) == 0);    // ...the cap still applies
}

TEST_CASE("rejection classes have distinct names for telemetry") {
  CHECK(std::string_view{to_string(Reject::None)} == "none");
  CHECK(std::string_view{to_string(Reject::TooShort)} == "too_short");
  CHECK(std::string_view{to_string(Reject::BadVersion)} == "bad_version");
  CHECK(std::string_view{to_string(Reject::UnknownType)} == "unknown_type");
  CHECK(std::string_view{to_string(Reject::ReservedFlag)} == "reserved_flag");
  CHECK(std::string_view{to_string(Reject::BadLength)} == "bad_length");
}
