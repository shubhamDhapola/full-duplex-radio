#pragma once

#include <cstdint>

#include "radio/clock.hpp"

namespace radiobench {

// Blocks the calling thread for approximately `duration`.
//
// "Approximately" is load-bearing: nanosleep guarantees a minimum, not a
// maximum, and on a general-purpose OS it commonly overshoots by a few hundred
// microseconds to a millisecond. That is precisely why radio::Pacer computes
// departures from a fixed schedule rather than from the clock -- oversleep on one
// frame must not shift any later frame.
void sleep_us(radio::Micros duration);

// Ctrl-C and SIGTERM set a flag rather than terminating, so a run still prints
// its report. A benchmark that dies without reporting has wasted the run.
void install_signal_handlers();
[[nodiscard]] bool stop_requested();

}  // namespace radiobench
