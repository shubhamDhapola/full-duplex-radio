// Estimates the offset between our clock and a peer's, from PING/PONG probes.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "radio/clock.hpp"
#include "radio/histogram.hpp"

namespace radio {

// THE EXCHANGE (spec section 4.1)
//
//     A                                      B
//     | t1 -- PING ----------------------->  | t2
//     | t4 <---------------------- PONG --   | t3
//
// t1 and t4 are on our clock; t2 and t3 are on the peer's. The two clocks have
// unrelated epochs (see clock.hpp), so t2 - t1 is not a duration -- it is a
// duration plus an unknown constant.
//
// Writing theta for "peer clock minus our clock" and d1, d2 for the one-way
// delays:
//
//     t2 = t1 + d1 + theta
//     t4 = t3 + d2 - theta
//
// Adding those eliminates theta entirely:
//
//     rtt = d1 + d2 = (t4 - t1) - (t3 - t2)
//
// So RTT is measured *exactly*, with no synchronisation and no assumption. Note
// it also subtracts (t3 - t2), the time the peer spent thinking, so this is a
// network measurement rather than a request-latency measurement.
//
// Offset is harder. Two equations, three unknowns -- underdetermined. To get a
// number at all you must assume something about d1 and d2, and the only
// available assumption is that they are equal:
//
//     theta = ((t2 - t1) + (t3 - t4)) / 2        assuming d1 == d2 == rtt/2
//
// THAT ASSUMPTION IS THE ENTIRE ERROR BUDGET. If the true delays differ by
// epsilon, the offset is wrong by epsilon/2, and nothing in these four numbers
// can reveal it. Hence offset_uncertainty_us(): the worst-case error is rtt/2,
// which is what any one-way latency figure derived from this must be quoted
// with. A one-way latency of "38 ms" from a 12 ms RTT is really 38 +/- 6 ms.
//
// WHY THE BEST SAMPLE IS SELECTED, NOT AVERAGED
//
// The instinct is to average many samples to reduce noise. That is wrong here,
// and the reason is worth internalising: averaging suppresses *zero-mean noise*,
// but queueing delay is neither zero-mean nor symmetric. It is non-negative and
// it accumulates on one direction of the path at a time. A burst of queueing on
// the A->B leg pushes every affected sample's offset the same way, so averaging
// bakes the bias in rather than cancelling it.
//
// The sample with the lowest RTT is the one that queued least, so it is closest
// to pure propagation delay and therefore closest to the symmetry the estimator
// assumes. Keeping a window and picking the minimum is what NTP does, for
// exactly this reason.
//
// The general principle: when your error source is a one-sided bias rather than
// noise, filter by *selecting* a good sample, not by averaging bad ones.
class ClockSync {
 public:
  // Eight probes at roughly 1/second is a few seconds of history -- long enough
  // to catch a quiet moment on the path, short enough that clock drift has not
  // meaningfully changed the offset across the window.
  static constexpr std::size_t kWindowSize = 8;

  // Probes slower than this are treated as bogus rather than as a very bad
  // path: on a LAN it means a stale or duplicated PONG matched the wrong probe.
  static constexpr std::int64_t kMaxPlausibleRttUs = 2'000'000;

  // Drift is a rate, so it needs a long baseline before the offset error
  // (+/- rtt/2 at each end) stops dominating the slope. See drift_ppm().
  static constexpr std::int64_t kMinDriftSpanUs = 10'000'000;

  struct Sample {
    std::int64_t rtt_us = 0;
    // Peer clock minus our clock. Add it to one of our timestamps to get the
    // peer's, subtract to go the other way.
    std::int64_t offset_us = 0;
    // t4: when this exchange completed, on our clock.
    Micros local_time_us = 0;
    bool valid = false;
  };

  // Feeds one completed exchange. Returns the computed sample, or an invalid
  // one if the timestamps are self-inconsistent.
  Sample observe(Micros t1, Micros t2, Micros t3, Micros t4) noexcept;

  void reset() noexcept;

  [[nodiscard]] bool has_estimate() const noexcept { return accepted_ > 0; }
  [[nodiscard]] std::uint64_t accepted() const noexcept { return accepted_; }

  // Exchanges discarded as impossible. Non-zero on a LAN is a real finding:
  // either a peer is mis-implementing PONG, or responses are being matched to
  // the wrong probe.
  [[nodiscard]] std::uint64_t rejected() const noexcept { return rejected_; }

  // The lowest-RTT sample currently in the window. This is the estimate to use.
  [[nodiscard]] Sample best() const noexcept;

  [[nodiscard]] std::int64_t offset_us() const noexcept { return best().offset_us; }
  [[nodiscard]] std::int64_t rtt_us() const noexcept { return best().rtt_us; }

  // Worst-case error on offset_us(), from the unobservable path asymmetry.
  // Always quote it alongside any one-way figure.
  [[nodiscard]] std::int64_t offset_uncertainty_us() const noexcept;

  // Translate between clock domains using the current best offset.
  [[nodiscard]] Micros remote_to_local(Micros remote_us) const noexcept;
  [[nodiscard]] Micros local_to_remote(Micros local_us) const noexcept;

  // Coarse relative frequency error between the two oscillators, in parts per
  // million, from two points: the best sample of the first full window, and the
  // best sample now.
  //
  // Two points is deliberately crude. It exists so drift is *visible* during
  // M0 rather than discovered in M4, and because even a rough number settles
  // whether it matters: at 20 ppm a call accumulates 72 ms of buffer error per
  // hour, which a jitter buffer must absorb or correct. M4 replaces this with a
  // proper regression over buffer occupancy.
  [[nodiscard]] bool has_drift_estimate() const noexcept;
  [[nodiscard]] double drift_ppm() const noexcept;

  // RTT distribution, for the P50/P95/P99 that a mean would hide (lesson 07).
  [[nodiscard]] const Histogram& rtt_histogram() const noexcept {
    return rtt_histogram_;
  }

 private:
  std::array<Sample, kWindowSize> window_{};
  std::size_t next_slot_ = 0;
  std::uint64_t accepted_ = 0;
  std::uint64_t rejected_ = 0;
  Sample drift_anchor_{};
  Histogram rtt_histogram_{};
};

}  // namespace radio
