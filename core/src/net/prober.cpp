#include "radio/prober.hpp"

namespace radio {

void Prober::start(const Config& config, Micros now) noexcept {
  config_ = config;
  reset();
  pacer_ = Pacer(config_.interval_us);
  pacer_.start(now);
  started_ = true;
}

void Prober::reset() noexcept {
  slots_ = {};
  next_request_id_ = 1;
  pacer_.reset();
  sync_.reset();
  stats_ = Stats{};
  last_reply_us_ = 0;
  last_rtt_us_ = 0;
  started_ = false;
}

Prober::Slot* Prober::find(std::uint32_t request_id) noexcept {
  for (auto& slot : slots_) {
    if (slot.outstanding && slot.request_id == request_id) return &slot;
  }
  return nullptr;
}

std::size_t Prober::outstanding() const noexcept {
  std::size_t count = 0;
  for (const auto& slot : slots_) {
    if (slot.outstanding) ++count;
  }
  return count;
}

Prober::Probe Prober::due_probe(Micros now, ByteSpan out) noexcept {
  if (!started_ || !pacer_.due(now)) return Probe{};

  Slot* slot = nullptr;
  for (auto& candidate : slots_) {
    if (!candidate.outstanding) {
      slot = &candidate;
      break;
    }
  }

  // Every slot outstanding means a whole timeout's worth of probes has gone
  // unanswered. This probe is skipped rather than evicting the oldest, because
  // evicting would make that probe vanish instead of being counted as a
  // timeout -- and the timeout count is the number the link indicator reads.
  //
  // The pacer is still advanced, so the schedule does not accumulate a backlog
  // of skipped slots to fire off in a burst the moment one frees up.
  if (slot == nullptr) {
    pacer_.advance(now);
    return Probe{};
  }

  proto::ControlHeader header;
  header.type = proto::Type::Ping;
  header.request_id = next_request_id_;
  // `now` is t1. Nothing in here reads the clock: every timestamp arrives as
  // an argument, which is what lets the whole class be driven from a test with
  // made-up times and no network.
  header.send_time_us = now;

  const std::size_t length = proto::encode_control(header, ByteView{}, out);
  if (length == 0) return Probe{};

  slot->request_id = next_request_id_;
  slot->sent_us = now;
  slot->outstanding = true;

  pacer_.advance(now);
  ++stats_.sent;

  return Probe{length, next_request_id_++};
}

void Prober::withdraw(std::uint32_t request_id) noexcept {
  Slot* slot = find(request_id);
  if (slot == nullptr) return;
  slot->outstanding = false;
  ++stats_.withdrawn;
  --stats_.sent;  // it never went out, so it was never sent
}

void Prober::on_pong(const proto::ControlPacket& packet, Micros t4) noexcept {
  Slot* slot = find(packet.header.request_id);
  if (slot == nullptr) {
    // A reply to a probe already given up on, or one that never existed.
    // Discarded rather than paired with some other probe's t1 -- doing that
    // would fabricate an RTT out of two unrelated moments, and the result would
    // look entirely plausible.
    ++stats_.stale;
    return;
  }

  const auto body = proto::parse_pong_body(packet.body);
  if (!body) {
    ++stats_.malformed;
    slot->outstanding = false;
    return;
  }

  // The echo is checked, never used. See the comment on Slot.
  if (body.value.orig_t1 != slot->sent_us) ++stats_.bad_echo;

  const Micros t1 = slot->sent_us;
  const Micros t2 = body.value.recv_t2;
  const Micros t3 = packet.header.send_time_us;
  slot->outstanding = false;

  const auto sample = sync_.observe(t1, t2, t3, t4);
  if (!sample.valid) {
    ++stats_.impossible;
    return;
  }

  ++stats_.replied;
  last_reply_us_ = t4;
  last_rtt_us_ = sample.rtt_us;
}

void Prober::expire(Micros now) noexcept {
  for (auto& slot : slots_) {
    // Micros is unsigned, so `now < sent_us` would wrap to an enormous elapsed
    // time and retire every probe at once. It cannot happen with a monotonic
    // clock, but it can happen in a test, and a guard is cheaper than a
    // confusing failure.
    if (slot.outstanding && now >= slot.sent_us &&
        now - slot.sent_us >= config_.timeout_us) {
      slot.outstanding = false;
      ++stats_.timed_out;
    }
  }
}

std::size_t encode_pong_for(const proto::ControlPacket& ping, Micros t2,
                            Micros t3, ByteSpan out) noexcept {
  proto::PongBody body;
  body.orig_t1 = ping.header.send_time_us;
  body.recv_t2 = t2;

  std::array<std::byte, proto::kPongBodySize> encoded_body{};
  if (proto::encode_pong_body(body, encoded_body) == 0) return 0;

  proto::ControlHeader header;
  header.type = proto::Type::Pong;
  header.request_id = ping.header.request_id;
  header.send_time_us = t3;

  return proto::encode_control(
      header, ByteView{encoded_body.data(), encoded_body.size()}, out);
}

}  // namespace radio
