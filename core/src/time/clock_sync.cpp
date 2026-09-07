#include "radio/clock_sync.hpp"

namespace radio {

ClockSync::Sample ClockSync::observe(Micros t1, Micros t2, Micros t3,
                                     Micros t4) noexcept {
  // Signed arithmetic throughout. t1 and t4 are on our clock while t2 and t3
  // are on the peer's, so (t2 - t1) crosses clock domains and is legitimately
  // negative whenever the peer's epoch is lower than ours. Doing this in Micros
  // (unsigned) would turn a small negative into ~1.8e19 and the estimate would
  // be silently absurd -- the same failure mode as the loss counter in
  // seq_tracker and in any duration taken from a steppable clock.
  const auto T1 = static_cast<std::int64_t>(t1);
  const auto T2 = static_cast<std::int64_t>(t2);
  const auto T3 = static_cast<std::int64_t>(t3);
  const auto T4 = static_cast<std::int64_t>(t4);

  Sample sample;

  // Each clock is monotonic within its own domain, so these two orderings must
  // hold. A violation means the peer's timestamps are wrong or a PONG was
  // matched to the wrong probe -- either way the sample is not salvageable.
  if (T4 < T1 || T3 < T2) {
    ++rejected_;
    return sample;
  }

  const std::int64_t rtt = (T4 - T1) - (T3 - T2);

  // A negative RTT is physically impossible; it means the peer reported
  // spending longer on the request than the whole exchange took.
  if (rtt < 0 || rtt > kMaxPlausibleRttUs) {
    ++rejected_;
    return sample;
  }

  sample.rtt_us = rtt;
  sample.offset_us = ((T2 - T1) + (T3 - T4)) / 2;
  sample.local_time_us = t4;
  sample.valid = true;

  window_[next_slot_] = sample;
  next_slot_ = (next_slot_ + 1) % kWindowSize;
  ++accepted_;
  rtt_histogram_.record(static_cast<std::uint64_t>(rtt));

  // Freeze a drift baseline once the first window is full, rather than tracking
  // the best-ever sample. If the anchor kept being replaced by later samples,
  // the time baseline would keep shortening and the slope would never
  // stabilise.
  if (!drift_anchor_.valid && accepted_ >= kWindowSize) {
    drift_anchor_ = best();
  }

  return sample;
}

void ClockSync::reset() noexcept { *this = ClockSync{}; }

ClockSync::Sample ClockSync::best() const noexcept {
  Sample winner;
  for (const auto& candidate : window_) {
    if (!candidate.valid) continue;
    if (!winner.valid || candidate.rtt_us < winner.rtt_us) winner = candidate;
  }
  return winner;
}

std::int64_t ClockSync::offset_uncertainty_us() const noexcept {
  const auto sample = best();
  if (!sample.valid) return 0;
  // The offset assumed d1 == d2. If the true split is entirely one-sided the
  // error reaches rtt/2, and nothing observable distinguishes the two cases.
  return sample.rtt_us / 2;
}

Micros ClockSync::remote_to_local(Micros remote_us) const noexcept {
  const auto sample = best();
  if (!sample.valid) return remote_us;
  return static_cast<Micros>(static_cast<std::int64_t>(remote_us) -
                             sample.offset_us);
}

Micros ClockSync::local_to_remote(Micros local_us) const noexcept {
  const auto sample = best();
  if (!sample.valid) return local_us;
  return static_cast<Micros>(static_cast<std::int64_t>(local_us) +
                             sample.offset_us);
}

bool ClockSync::has_drift_estimate() const noexcept {
  if (!drift_anchor_.valid) return false;
  const auto current = best();
  if (!current.valid) return false;
  const auto span = static_cast<std::int64_t>(current.local_time_us) -
                    static_cast<std::int64_t>(drift_anchor_.local_time_us);
  return span >= kMinDriftSpanUs;
}

double ClockSync::drift_ppm() const noexcept {
  if (!has_drift_estimate()) return 0.0;
  const auto current = best();
  const auto span = static_cast<double>(
      static_cast<std::int64_t>(current.local_time_us) -
      static_cast<std::int64_t>(drift_anchor_.local_time_us));
  const auto change =
      static_cast<double>(current.offset_us - drift_anchor_.offset_us);
  // Microseconds of offset change per microsecond elapsed, scaled to ppm.
  return change * 1'000'000.0 / span;
}

}  // namespace radio
