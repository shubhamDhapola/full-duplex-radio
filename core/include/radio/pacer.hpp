// Spaces packet departures so a stream leaves at its natural frame rate.
#pragma once

#include <cstdint>

#include "radio/clock.hpp"

namespace radio {

// WHY PACE AT ALL
//
// The lazy send path accumulates whatever is ready and pushes it out in one go.
// For voice that produces bursts of 3-5 datagrams followed by silence, and it
// hurts in two ways:
//
//   1. A burst can overrun the queue on a switch or an access point, which
//      drops packets that a paced stream would have delivered. Self-inflicted
//      loss.
//
//   2. Worse, and less obvious: the receiver sees the datagrams arrive clumped
//      together. Its interarrival jitter estimator cannot tell "the sender
//      bursted" from "the network delayed these", so it reports high jitter, and
//      an adaptive jitter buffer responds by getting deeper. That extra depth is
//      real added latency, paid on every frame for the rest of the call, to
//      absorb variance the sender created.
//
// So pacing is a latency optimisation, not politeness.
//
// THE DRIFT TRAP
//
// The obvious way to schedule the next departure is:
//
//     next = now + interval;          // WRONG
//
// `now` is always slightly late -- the thread woke up a bit after it was due.
// Adding the interval to a late value bakes that lateness in permanently, and
// the error compounds. At 20 ms frames, being 300 us late each time drifts by
// 15 ms every second, so after a minute the stream is nearly a second behind
// where it should be and the receiver's buffer has silently drained.
//
// The fix is to advance the *schedule*, not the clock reading:
//
//     next += interval;               // right
//
// Lateness on one frame then has no effect on any later frame.
//
// THE CATCH-UP TRAP
//
// Advancing the schedule blindly creates the opposite problem. If the thread is
// descheduled for 200 ms, ten frame slots pass, and ten frames are immediately
// "due". Sending them back to back produces exactly the burst pacing exists to
// prevent -- and it is worse than the original burst, because those frames are
// already late and will be discarded by the receiver's playout deadline anyway.
//
// So past a threshold the pacer gives up on the old schedule and resynchronises.
// Frames that were due during the stall are simply not sent; in real-time media
// that is the correct choice, because a frame delivered late is worth no more
// than one never sent.
class Pacer {
 public:
  Pacer() noexcept = default;

  // `max_catchup_us` of 0 selects two intervals, which tolerates ordinary
  // scheduling noise while still refusing to burst after a real stall.
  explicit Pacer(Micros interval_us, Micros max_catchup_us = 0) noexcept;

  void start(Micros now_us) noexcept;
  void reset() noexcept;

  [[nodiscard]] bool started() const noexcept { return started_; }
  [[nodiscard]] Micros interval_us() const noexcept { return interval_; }

  // When the next packet is scheduled to leave.
  [[nodiscard]] Micros next_departure_us() const noexcept { return next_; }

  // Microseconds to wait before the next departure; 0 if due or overdue. This
  // is what a send loop passes to poll() as its timeout.
  [[nodiscard]] Micros delay_until_due_us(Micros now_us) const noexcept;

  [[nodiscard]] bool due(Micros now_us) const noexcept;

  // Records a departure and moves the schedule on by exactly one interval,
  // resynchronising if we have fallen further behind than max_catchup allows.
  //
  // The tolerance is measured against the schedule *after* advancing, so the
  // resync condition is `now > next + interval + max_catchup`. A whole slot must
  // therefore be missed before any resync happens, however tight the tolerance
  // is set -- being merely late for the current slot is normal and is absorbed.
  void advance(Micros now_us) noexcept;

  // Times the schedule was abandoned because the sender fell too far behind.
  // Non-zero in production means the send thread is being starved, which is a
  // real finding rather than a curiosity.
  [[nodiscard]] std::uint64_t resyncs() const noexcept { return resyncs_; }

  // Departures recorded.
  [[nodiscard]] std::uint64_t departures() const noexcept { return departures_; }

 private:
  Micros interval_ = 0;
  Micros max_catchup_ = 0;
  Micros next_ = 0;
  std::uint64_t resyncs_ = 0;
  std::uint64_t departures_ = 0;
  bool started_ = false;
};

}  // namespace radio
