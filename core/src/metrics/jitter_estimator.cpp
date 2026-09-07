#include "radio/jitter_estimator.hpp"

#include <cstdlib>

#include "radio/serial.hpp"

namespace radio {

void JitterEstimator::anchor(std::uint32_t timestamp,
                             Micros arrival_us) noexcept {
  prev_timestamp_ = timestamp;
  prev_arrival_ = us_to_samples(arrival_us);
  anchored_ = true;
}

void JitterEstimator::reanchor(std::uint32_t timestamp,
                               Micros arrival_us) noexcept {
  anchor(timestamp, arrival_us);
}

void JitterEstimator::observe(std::uint32_t timestamp,
                              Micros arrival_us) noexcept {
  const std::uint64_t arrival = us_to_samples(arrival_us);

  if (!anchored_) {
    // Nothing to difference against yet. The first packet of a stream
    // establishes the reference and contributes no sample.
    anchor(timestamp, arrival_us);
    return;
  }

  // Arrival is on our own monotonic clock, so this is always non-negative.
  const auto arrival_delta = static_cast<std::int64_t>(arrival - prev_arrival_);

  // Timestamps are 32-bit and wrap, so this must be serial arithmetic rather
  // than plain subtraction (see serial.hpp). A stream that happened to start
  // near the wrap would otherwise report a delta of about 4.3 billion samples
  // -- roughly a day of audio -- and the jitter estimate would never recover.
  const std::int64_t timestamp_delta =
      serial::distance(prev_timestamp_, timestamp);

  // The offset between the two clocks cancels here. See the header.
  const std::int64_t delta = arrival_delta - timestamp_delta;

  last_delta_ = delta;
  const std::int64_t abs_delta = delta < 0 ? -delta : delta;
  if (abs_delta > peak_abs_delta_) peak_abs_delta_ = abs_delta;

  jitter_ += (static_cast<double>(abs_delta) - jitter_) /
             static_cast<double>(1 << kGainShift);

  prev_timestamp_ = timestamp;
  prev_arrival_ = arrival;
  ++samples_;
}

void JitterEstimator::reset() noexcept { *this = JitterEstimator{}; }

double JitterEstimator::jitter_us() const noexcept {
  // 48 kHz sample units to microseconds: divide by 48 per microsecond.
  return jitter_ * 1'000.0 / 48.0;
}

double JitterEstimator::jitter_ms() const noexcept {
  return jitter_us() / 1'000.0;
}

}  // namespace radio
