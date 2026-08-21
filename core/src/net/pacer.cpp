#include "radio/pacer.hpp"

namespace radio {

Pacer::Pacer(Micros interval_us, Micros max_catchup_us) noexcept
    : interval_(interval_us),
      max_catchup_(max_catchup_us != 0 ? max_catchup_us : interval_us * 2) {}

void Pacer::start(Micros now_us) noexcept {
  next_ = now_us;
  started_ = true;
}

void Pacer::reset() noexcept {
  const Micros interval = interval_;
  const Micros catchup = max_catchup_;
  *this = Pacer{};
  interval_ = interval;
  max_catchup_ = catchup;
}

Micros Pacer::delay_until_due_us(Micros now_us) const noexcept {
  if (!started_ || now_us >= next_) return 0;
  return next_ - now_us;
}

bool Pacer::due(Micros now_us) const noexcept {
  return started_ && now_us >= next_;
}

void Pacer::advance(Micros now_us) noexcept {
  if (!started_) {
    start(now_us);
  }

  ++departures_;

  // Advance the schedule, not the clock reading. See the header: adding the
  // interval to `now` would fold this frame's lateness into every frame after
  // it.
  next_ += interval_;

  // If we are further behind than the tolerance allows, the schedule is lost.
  // Resynchronising drops the frames that were due during the stall rather than
  // sending them in a burst -- they would arrive past their playout deadline in
  // any case, so bursting would trade real harm for no benefit.
  if (now_us > next_ && (now_us - next_) > max_catchup_) {
    next_ = now_us;
    ++resyncs_;
  }
}

}  // namespace radio
