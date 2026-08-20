// Time base for the whole core.
#pragma once

#include <cstdint>

namespace radio {

// Microseconds since an arbitrary, per-process epoch. Only differences are
// meaningful; the absolute value means nothing and must never be logged as if
// it were a date.
using Micros = std::uint64_t;

// Media timing is expressed in 48 kHz sample units on the wire (spec §3.3),
// because that is the unit the codec and the audio device actually work in.
// Converting to microseconds is for humans and telemetry, not for the pipeline.
inline constexpr std::uint32_t kSampleRateHz = 48'000;
inline constexpr std::uint32_t kFrameMs = 20;
inline constexpr std::uint32_t kFrameSamples = kSampleRateHz / 1'000 * kFrameMs;

static_assert(kFrameSamples == 960, "20 ms at 48 kHz is 960 samples");

// 48000 / 1e6 reduces to 48 / 1000, so these stay in integer arithmetic. The
// multiply overflows only past ~12,000 years of uptime.
[[nodiscard]] constexpr std::uint64_t us_to_samples(Micros us) noexcept {
  return us * 48u / 1'000u;
}

[[nodiscard]] constexpr Micros samples_to_us(std::uint64_t samples) noexcept {
  return samples * 1'000u / 48u;
}

// Reads the system's monotonic clock.
//
// Three clocks are plausible here and only one is correct:
//
//   Wall clock (CLOCK_REALTIME, gettimeofday). Wrong. It is adjusted by NTP,
//   by the user, and by timezone and DST changes. An adjustment mid-call makes
//   an interval measurement negative or inserts a jump of hours, and every
//   downstream latency figure becomes garbage. A negative duration is also the
//   fastest way to make unsigned arithmetic produce an enormous positive
//   number, which then propagates silently.
//
//   CLOCK_MONOTONIC. Close, but NTP slews it — the clock's *rate* is adjusted
//   to converge on true time. Over a long call that slew is precisely the kind
//   of small, systematic frequency error that clock-drift compensation exists
//   to measure, so measuring with a slewed clock would fold the fix into the
//   measurement.
//
//   CLOCK_MONOTONIC_RAW. What this uses: the raw hardware counter, never
//   stepped and never slewed. Interval measurements are exactly what the
//   oscillator saw.
//
// The tradeoff of _RAW is that it does not advance while the device is
// suspended. That is acceptable and arguably correct: a suspended phone is not
// holding a voice call, and if it suspends mid-call the session is over for
// reasons no clock can paper over. If a use case ever needs to span suspend,
// CLOCK_BOOTTIME is the one to reach for, and the choice should be explicit.
[[nodiscard]] Micros now_us() noexcept;

// Nanosecond resolution from the same source, for measuring things short enough
// that a microsecond is a coarse unit — an Opus decode call, or a lock-free
// queue handoff.
[[nodiscard]] std::uint64_t now_ns() noexcept;

}  // namespace radio
