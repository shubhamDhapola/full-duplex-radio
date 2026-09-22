// Asynchronous PING/PONG probing: RTT, clock offset and liveness, without
// waiting for anything.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "radio/bytes.hpp"
#include "radio/clock.hpp"
#include "radio/clock_sync.hpp"
#include "radio/pacer.hpp"
#include "radio/proto.hpp"

namespace radio {

// WHY THIS IS NOT JUST `radiobench ping`
//
// The host tool probes synchronously: send, then sit in a receive loop until
// the PONG arrives or the deadline passes. That is correct when measuring is
// the only job.
//
// A live endpoint cannot do that. The same thread is draining a microphone and
// servicing media, and blocking for up to a second would stall both. So the
// waiting has to go: probes are fired on a schedule, the unanswered ones live
// in a small table, and replies are matched whenever they happen to turn up in
// the ordinary receive path.
//
// It is the same measurement. Only the waiting is different -- which is exactly
// why the matching has to be written down carefully, because with no blocking
// read to pair a reply with its probe, nothing but the table says which t1 a
// given t4 belongs to. Pairing a reply with the wrong t1 does not fail loudly;
// it produces a plausible RTT that is simply wrong.
//
// WHAT THIS DOES NOT OWN
//
// No socket. Probes come out as bytes and replies go in as parsed packets, so
// the whole class is a function of (time in, bytes in) and can be tested on the
// host with no network at all. The endpoint that owns the socket does the I/O.
class Prober {
 public:
  struct Config {
    // Spacing between probes, and how long to keep waiting for each reply.
    // The defaults match `radiobench ping`, deliberately: a figure measured
    // here and a figure measured by the host tool are only comparable if they
    // were sampled on the same schedule.
    Micros interval_us = 200'000;
    Micros timeout_us = 1'000'000;
  };

  // A 200 ms interval with 1 s of patience leaves five probes outstanding at
  // once. Eight leaves headroom and keeps the table a linear scan of one cache
  // line, which is cheaper than any lookup structure at this size.
  static constexpr std::size_t kMaxOutstanding = 8;

  struct Stats {
    std::uint64_t sent = 0;
    std::uint64_t replied = 0;
    std::uint64_t timed_out = 0;
    // A PONG matching no outstanding probe. Usually a late reply to one already
    // given up on -- harmless, but counted, because a large number means the
    // timeout is set shorter than the path actually is.
    std::uint64_t stale = 0;
    // A PONG that echoed back a t1 we never sent. The peer is buggy, or
    // something between us is rewriting packets.
    std::uint64_t bad_echo = 0;
    // An exchange whose four timestamps cannot describe a real round trip.
    // Non-zero on a LAN is a finding, not noise.
    std::uint64_t impossible = 0;
    // A reply that parsed as a PONG but carried an unusable body.
    std::uint64_t malformed = 0;
    // Probes the caller reported as never having left the socket. Kept apart
    // from `timed_out` so local failure is not read as network loss.
    std::uint64_t withdrawn = 0;
  };

  // What due_probe() produced. The request id comes back so a caller whose send
  // failed can withdraw exactly that probe rather than guessing at the latest.
  struct Probe {
    std::size_t length = 0;
    std::uint32_t request_id = 0;

    [[nodiscard]] constexpr bool ready() const noexcept { return length != 0; }
  };

  void start(const Config& config, Micros now) noexcept;
  void reset() noexcept;

  [[nodiscard]] bool started() const noexcept { return started_; }

  // Encodes the next probe if one is due, and registers it as outstanding.
  // Returns a zero length when nothing is due, when the table is full, or when
  // `out` is too small.
  //
  // `now` becomes t1, so read it immediately before the call: everything
  // between that read and the caller's sendto() is charged to the network by
  // the RTT formula. Nothing in this class reads the clock itself, which is
  // what lets a test drive it with invented times and no socket.
  //
  // `out` needs proto::kControlHeaderSize bytes.
  [[nodiscard]] Probe due_probe(Micros now, ByteSpan out) noexcept;

  // The caller's send failed, so this probe never left. Retires it without
  // counting it as a timeout -- see Stats::withdrawn.
  void withdraw(std::uint32_t request_id) noexcept;

  // Feeds a reply. `t4` must be the arrival timestamp taken at the syscall, not
  // a clock read from after parsing: everything between the two is attributed
  // to the network by the RTT formula.
  void on_pong(const proto::ControlPacket& packet, Micros t4) noexcept;

  // Retires probes past their deadline. Cheap, and safe to call every loop.
  void expire(Micros now) noexcept;

  [[nodiscard]] const ClockSync& sync() const noexcept { return sync_; }
  [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

  // t4 of the most recent accepted exchange, or 0 if none. This is the only
  // honest basis for a liveness indicator: UDP has no connection to ask about,
  // so "reachable" can only mean "answered recently".
  [[nodiscard]] Micros last_reply_us() const noexcept { return last_reply_us_; }

  // RTT of the most recent accepted exchange. sync().best() gives the
  // lowest-RTT sample in the window instead, which is the one to quote -- see
  // clock_sync.hpp on why the minimum is selected rather than the mean taken.
  [[nodiscard]] std::int64_t last_rtt_us() const noexcept {
    return last_rtt_us_;
  }

  [[nodiscard]] std::size_t outstanding() const noexcept;

 private:
  // One unanswered probe.
  //
  // `sent_us` is kept here rather than taken from the PONG's echo. The
  // responder does send t1 back, and using it would save these bytes -- but it
  // would let the peer choose a number that lands inside our own measurement.
  // A buggy peer would then produce a wrong RTT that looks right, and a hostile
  // one could name it. The echo is compared instead, which is what
  // Stats::bad_echo counts.
  struct Slot {
    std::uint32_t request_id = 0;
    Micros sent_us = 0;
    bool outstanding = false;
  };

  [[nodiscard]] Slot* find(std::uint32_t request_id) noexcept;

  Config config_{};
  std::array<Slot, kMaxOutstanding> slots_{};
  std::uint32_t next_request_id_ = 1;
  Pacer pacer_;
  ClockSync sync_;
  Stats stats_{};
  Micros last_reply_us_ = 0;
  std::int64_t last_rtt_us_ = 0;
  bool started_ = false;
};

// Builds the PONG owed for a received PING, ready to send back to whoever
// asked.
//
// `t2` is when the PING arrived, taken at the syscall. `t3` is when the reply
// leaves, and must be read as late as possible -- immediately before this call
// -- because the gap between the two is the responder's own thinking time and
// the prober's RTT formula subtracts it out. Getting t3 late is what stops a
// slow responder from making the network look bad on somebody else's screen.
//
// `out` needs proto::kControlHeaderSize + proto::kPongBodySize bytes. Returns
// the length written, or 0.
[[nodiscard]] std::size_t encode_pong_for(const proto::ControlPacket& ping,
                                          Micros t2, Micros t3,
                                          ByteSpan out) noexcept;

}  // namespace radio
