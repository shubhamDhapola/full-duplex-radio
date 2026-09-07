// Interarrival jitter, per RFC 3550 §6.4.1.
#pragma once

#include <cstdint>

#include "radio/clock.hpp"

namespace radio {

// Estimates how much the network's delivery timing is varying.
//
// The useful property, and the reason this can be measured on day one while
// one-way latency cannot: it needs no clock synchronisation at all.
//
// Define transit time as arrival-on-our-clock minus the sender's timestamp:
//
//     transit_i = R_i - S_i
//
// That value is wrong. It is the true one-way delay plus D, the unknown
// constant offset between two unsynchronised monotonic clocks (see
// clock.hpp). But take the difference between consecutive packets:
//
//     delta(i-1, i) = transit_i - transit_{i-1}
//                   = (true_delay_i + D) - (true_delay_{i-1} + D)
//                   = true_delay_i - true_delay_{i-1}
//
// The offset cancels algebraically, not statistically. What remains is the
// exact change in one-way delay. Smoothing |delta| with an exponential moving
// average gives the standard jitter estimate:
//
//     J += (|delta| - J) / 16
//
// The gain of 1/16 is a power of two (a shift, not a divide) and gives a time
// constant of roughly 16 packets — about 320 ms at 20 ms frames. Long enough to
// ignore single-packet noise, short enough to react within a talkspurt.
//
// Everything here is in 48 kHz sample units, matching the wire timestamp, so no
// conversion happens on the hot path. `double` is used rather than RFC 3550's
// suggested scaled integer because this runs on the network receive thread, not
// in the audio callback, so there is no reason to avoid floating point.
class JitterEstimator {
 public:
  static constexpr int kGainShift = 4;  // J += (|delta| - J) / 16

  // One received packet. `timestamp` is the wire field; `arrival_us` is a local
  // monotonic reading taken as close to the recvfrom() as possible — every
  // microsecond of delay between the syscall and this call is measured as
  // network jitter that isn't there.
  void observe(std::uint32_t timestamp, Micros arrival_us) noexcept;

  // Re-anchor without contributing a sample. Call this instead of observe() on
  // a packet flagged TALKSPURT_START.
  //
  // Across a silence gap the timing relationship is meaningless: the sender
  // stopped pacing, its encoder may have been reset, and the timestamp jump
  // reflects the length of the silence rather than anything the network did. A
  // single spurious delta there would dominate the average for the next ~16
  // packets, so the jitter estimate would spike at the start of every
  // talkspurt — exactly when an adaptive jitter buffer is deciding how deep to
  // be, and exactly when a wrong answer is most expensive.
  void reanchor(std::uint32_t timestamp, Micros arrival_us) noexcept;

  void reset() noexcept;

  [[nodiscard]] bool has_estimate() const noexcept { return samples_ > 0; }
  [[nodiscard]] std::uint64_t samples() const noexcept { return samples_; }

  [[nodiscard]] double jitter_samples() const noexcept { return jitter_; }
  [[nodiscard]] double jitter_us() const noexcept;
  [[nodiscard]] double jitter_ms() const noexcept;

  // Most recent delta, signed: positive means this packet's transit was longer
  // than the previous one's. Sign is discarded by the average but is useful
  // when reading a trace.
  [[nodiscard]] std::int64_t last_delta_samples() const noexcept {
    return last_delta_;
  }

  // Largest |delta| ever seen.
  //
  // Kept because J is an *average* and a jitter buffer has to survive the
  // *tail*. A stream with J = 3 ms and an occasional 60 ms excursion will
  // underrun repeatedly while its average looks healthy. Sizing a buffer from
  // the mean is the classic way to build something that works in testing and
  // glitches in the field — which is why the histogram exists alongside this.
  [[nodiscard]] std::int64_t peak_abs_delta_samples() const noexcept {
    return peak_abs_delta_;
  }

 private:
  void anchor(std::uint32_t timestamp, Micros arrival_us) noexcept;

  double jitter_ = 0.0;             // sample units
  std::uint64_t prev_arrival_ = 0;  // sample units
  std::uint32_t prev_timestamp_ = 0;
  std::int64_t last_delta_ = 0;
  std::int64_t peak_abs_delta_ = 0;
  std::uint64_t samples_ = 0;
  bool anchored_ = false;
};

}  // namespace radio
