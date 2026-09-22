// The asynchronous prober: matching replies to probes with no blocking read.
//
// Every timestamp here is invented, which is the point of the class taking them
// as arguments. Nothing in this file opens a socket or reads a clock, so a
// failure is a failure in the matching logic and cannot be a flaky network or a
// busy machine.
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>

#include "radio/prober.hpp"

using namespace radio;

namespace {

constexpr Micros kInterval = 200'000;
constexpr Micros kTimeout = 1'000'000;

Prober::Config config() {
  Prober::Config out;
  out.interval_us = kInterval;
  out.timeout_us = kTimeout;
  return out;
}

// Builds the reply a well-behaved peer would send for a probe we just encoded,
// exactly as `radiobench respond` does -- through the same encode_pong_for()
// the device uses, so this test also covers the responder half.
struct Reply {
  std::array<std::byte, proto::kMaxDatagram> bytes{};
  std::size_t length = 0;

  [[nodiscard]] proto::ControlPacket parsed() const {
    const auto packet = proto::parse_control(ByteView{bytes.data(), length});
    REQUIRE(packet.ok());
    return packet.value;
  }
};

Reply reply_to(ByteView ping_bytes, Micros t2, Micros t3) {
  const auto ping = proto::parse_control(ping_bytes);
  REQUIRE(ping.ok());

  Reply out;
  out.length = encode_pong_for(ping.value, t2, t3, out.bytes);
  REQUIRE(out.length != 0);
  return out;
}

}  // namespace

TEST_CASE("prober sends nothing before it is started", "[prober]") {
  Prober prober;
  std::array<std::byte, proto::kMaxDatagram> out{};

  REQUIRE_FALSE(prober.started());
  REQUIRE_FALSE(prober.due_probe(1'000'000, out).ready());
  REQUIRE(prober.stats().sent == 0);
}

TEST_CASE("prober paces probes at the configured interval", "[prober]") {
  Prober prober;
  std::array<std::byte, proto::kMaxDatagram> out{};

  const Micros t0 = 1'000'000;
  prober.start(config(), t0);

  // The first probe is due immediately; the schedule then advances by exactly
  // one interval, so being late on one probe does not shift the rest. That is
  // the pacer's contract and the reason it is used here rather than
  // `next = now + interval`.
  REQUIRE(prober.due_probe(t0, out).ready());
  REQUIRE_FALSE(prober.due_probe(t0 + kInterval / 2, out).ready());
  REQUIRE(prober.due_probe(t0 + kInterval, out).ready());
  REQUIRE(prober.stats().sent == 2);
  REQUIRE(prober.outstanding() == 2);
}

TEST_CASE("prober measures rtt from a well-formed reply", "[prober]") {
  Prober prober;
  std::array<std::byte, proto::kMaxDatagram> out{};

  const Micros t1 = 5'000'000;
  prober.start(config(), t1);

  const auto probe = prober.due_probe(t1, out);
  REQUIRE(probe.ready());

  // A peer whose clock is an hour ahead of ours, taking 300 us to answer.
  // The offset is unrelated to the delay, which is the whole point of the
  // four-timestamp exchange: rtt = (t4 - t1) - (t3 - t2) cancels it exactly.
  constexpr Micros kPeerOffset = 3'600'000'000;
  const Micros t2 = t1 + 2'000 + kPeerOffset;  // 2 ms out
  const Micros t3 = t2 + 300;                  // 300 us thinking
  const Micros t4 = t1 + 2'000 + 300 + 3'000;  // 3 ms back

  const Reply reply = reply_to(ByteView{out.data(), probe.length}, t2, t3);
  prober.on_pong(reply.parsed(), t4);

  REQUIRE(prober.stats().replied == 1);
  REQUIRE(prober.stats().stale == 0);
  REQUIRE(prober.stats().bad_echo == 0);
  REQUIRE(prober.stats().impossible == 0);

  // 2 ms out plus 3 ms back, with the peer's 300 us subtracted out.
  REQUIRE(prober.last_rtt_us() == 5'000);
  REQUIRE(prober.last_reply_us() == t4);
  REQUIRE(prober.outstanding() == 0);
  REQUIRE(prober.sync().has_estimate());
}

TEST_CASE("prober matches each reply to its own probe", "[prober]") {
  Prober prober;
  std::array<std::byte, proto::kMaxDatagram> first{};
  std::array<std::byte, proto::kMaxDatagram> second{};

  const Micros t0 = 2'000'000;
  prober.start(config(), t0);

  const auto a = prober.due_probe(t0, first);
  const auto b = prober.due_probe(t0 + kInterval, second);
  REQUIRE(a.ready());
  REQUIRE(b.ready());
  REQUIRE(a.request_id != b.request_id);

  // The second probe is answered first. With no blocking read to pair them,
  // only the request id says which t1 this t4 belongs to -- and getting it
  // wrong would produce an RTT of one whole interval that looks entirely
  // plausible.
  const Micros b_sent = t0 + kInterval;
  const Reply for_b = reply_to(ByteView{second.data(), b.length},
                               b_sent + 1'000, b_sent + 1'100);
  prober.on_pong(for_b.parsed(), b_sent + 2'000);

  REQUIRE(prober.stats().replied == 1);
  REQUIRE(prober.last_rtt_us() == 1'900);  // 2000 - 100, not 200'000-ish
  REQUIRE(prober.outstanding() == 1);      // the first probe is still open

  const Reply for_a =
      reply_to(ByteView{first.data(), a.length}, t0 + 4'000, t0 + 4'100);
  prober.on_pong(for_a.parsed(), t0 + 8'000);

  REQUIRE(prober.stats().replied == 2);
  REQUIRE(prober.last_rtt_us() == 7'900);
  REQUIRE(prober.outstanding() == 0);
}

TEST_CASE("prober retires a probe that is never answered", "[prober]") {
  Prober prober;
  std::array<std::byte, proto::kMaxDatagram> out{};

  const Micros t0 = 9'000'000;
  prober.start(config(), t0);
  const auto probe = prober.due_probe(t0, out);
  REQUIRE(probe.ready());

  prober.expire(t0 + kTimeout - 1);
  REQUIRE(prober.stats().timed_out == 0);
  REQUIRE(prober.outstanding() == 1);

  prober.expire(t0 + kTimeout);
  REQUIRE(prober.stats().timed_out == 1);
  REQUIRE(prober.outstanding() == 0);
}

TEST_CASE("prober discards a reply that arrives after its probe expired",
          "[prober]") {
  Prober prober;
  std::array<std::byte, proto::kMaxDatagram> out{};

  const Micros t0 = 4'000'000;
  prober.start(config(), t0);
  const auto probe = prober.due_probe(t0, out);
  prober.expire(t0 + kTimeout);
  REQUIRE(prober.stats().timed_out == 1);

  // The reply turns up anyway. Its t1 is long gone from the table, so pairing
  // it with anything would invent a measurement. It is counted and dropped.
  const Reply late =
      reply_to(ByteView{out.data(), probe.length}, t0 + 1'000, t0 + 1'100);
  prober.on_pong(late.parsed(), t0 + kTimeout + 5'000);

  REQUIRE(prober.stats().stale == 1);
  REQUIRE(prober.stats().replied == 0);
  REQUIRE(prober.last_rtt_us() == 0);
  REQUIRE_FALSE(prober.sync().has_estimate());
}

TEST_CASE("prober counts a disagreeing echo but does not use it", "[prober]") {
  Prober prober;
  std::array<std::byte, proto::kMaxDatagram> out{};

  const Micros t1 = 7'000'000;
  prober.start(config(), t1);
  const auto probe = prober.due_probe(t1, out);
  REQUIRE(probe.ready());

  // A peer that echoes a t1 we never sent -- here, one 50 ms earlier, which
  // would inflate the RTT by 50 ms if the echo were trusted. The local copy is
  // authoritative, so the measurement is unaffected and only the counter moves.
  const auto ping = proto::parse_control(ByteView{out.data(), probe.length});
  REQUIRE(ping.ok());
  proto::ControlPacket tampered = ping.value;
  tampered.header.send_time_us = t1 - 50'000;

  Reply reply;
  reply.length = encode_pong_for(tampered, t1 + 1'000, t1 + 1'100, reply.bytes);
  REQUIRE(reply.length != 0);

  prober.on_pong(reply.parsed(), t1 + 4'000);

  REQUIRE(prober.stats().bad_echo == 1);
  REQUIRE(prober.stats().replied == 1);
  REQUIRE(prober.last_rtt_us() == 3'900);  // from our t1, not the peer's
}

TEST_CASE("prober rejects an exchange that cannot have happened", "[prober]") {
  Prober prober;
  std::array<std::byte, proto::kMaxDatagram> out{};

  const Micros t1 = 6'000'000;
  prober.start(config(), t1);
  const auto probe = prober.due_probe(t1, out);

  // The peer claims to have spent longer thinking than the whole exchange took,
  // which would make the RTT negative. On a LAN this means PONGs are being
  // matched to the wrong probes somewhere, so it gets its own counter instead
  // of being folded into loss.
  const Reply impossible =
      reply_to(ByteView{out.data(), probe.length}, t1 + 1'000, t1 + 900'000);
  prober.on_pong(impossible.parsed(), t1 + 2'000);

  REQUIRE(prober.stats().impossible == 1);
  REQUIRE(prober.stats().replied == 0);
  REQUIRE_FALSE(prober.sync().has_estimate());
}

TEST_CASE("prober skips a probe rather than evicting an outstanding one",
          "[prober]") {
  Prober prober;
  std::array<std::byte, proto::kMaxDatagram> out{};

  const Micros t0 = 1'000'000;
  prober.start(config(), t0);

  // Fill the table: a silent peer and a timeout longer than the interval, which
  // is the normal case rather than a contrived one.
  for (std::size_t i = 0; i < Prober::kMaxOutstanding; ++i) {
    REQUIRE(prober.due_probe(t0 + i * kInterval, out).ready());
  }
  REQUIRE(prober.outstanding() == Prober::kMaxOutstanding);

  // Nothing is evicted, so the oldest probe survives to be counted as a
  // timeout. Losing it here would make the unanswered count -- the number the
  // link indicator reads -- quietly too low.
  const Micros full = t0 + Prober::kMaxOutstanding * kInterval;
  REQUIRE_FALSE(prober.due_probe(full, out).ready());
  REQUIRE(prober.stats().sent == Prober::kMaxOutstanding);

  prober.expire(t0 + kTimeout);
  REQUIRE(prober.stats().timed_out >= 1);

  // The schedule advanced through the skip, so the next probe goes out on the
  // ordinary cadence rather than a burst of catch-up firing at once.
  REQUIRE(prober.due_probe(full + kInterval, out).ready());
}

TEST_CASE("prober separates a failed send from a lost probe", "[prober]") {
  Prober prober;
  std::array<std::byte, proto::kMaxDatagram> out{};

  const Micros t0 = 3'000'000;
  prober.start(config(), t0);
  const auto probe = prober.due_probe(t0, out);
  REQUIRE(probe.ready());

  prober.withdraw(probe.request_id);

  // A probe the socket refused never reached the network, so counting it as a
  // timeout would blame the peer for a local failure.
  REQUIRE(prober.stats().withdrawn == 1);
  REQUIRE(prober.stats().sent == 0);
  REQUIRE(prober.outstanding() == 0);

  prober.expire(t0 + 2 * kTimeout);
  REQUIRE(prober.stats().timed_out == 0);
}

TEST_CASE("prober reports no estimate until a probe is answered", "[prober]") {
  Prober prober;
  std::array<std::byte, proto::kMaxDatagram> out{};

  prober.start(config(), 0);
  REQUIRE(prober.due_probe(0, out).ready());

  // last_reply_us() of 0 is what a liveness indicator reads as "has never
  // answered", so it must stay 0 while probes are merely outstanding.
  REQUIRE(prober.last_reply_us() == 0);
  REQUIRE_FALSE(prober.sync().has_estimate());
}

TEST_CASE("encode_pong_for echoes the prober's own fields", "[prober]") {
  proto::ControlHeader header;
  header.type = proto::Type::Ping;
  header.request_id = 0xABCD1234;
  header.send_time_us = 999'111'222;

  std::array<std::byte, proto::kMaxDatagram> ping{};
  const std::size_t ping_length =
      proto::encode_control(header, ByteView{}, ping);
  REQUIRE(ping_length != 0);

  const auto parsed = proto::parse_control(ByteView{ping.data(), ping_length});
  REQUIRE(parsed.ok());

  std::array<std::byte, proto::kMaxDatagram> pong{};
  const std::size_t pong_length =
      encode_pong_for(parsed.value, 1'000, 1'400, pong);
  REQUIRE(pong_length == proto::kControlHeaderSize + proto::kPongBodySize);

  const auto reply = proto::parse_control(ByteView{pong.data(), pong_length});
  REQUIRE(reply.ok());
  REQUIRE(reply.value.header.type == proto::Type::Pong);

  // The request id comes back untouched -- it is the only thing that lets the
  // prober pair this reply with a probe.
  REQUIRE(reply.value.header.request_id == header.request_id);
  REQUIRE(reply.value.header.send_time_us == 1'400);

  const auto body = proto::parse_pong_body(reply.value.body);
  REQUIRE(body.ok());
  REQUIRE(body.value.orig_t1 == header.send_time_us);
  REQUIRE(body.value.recv_t2 == 1'000);
}
