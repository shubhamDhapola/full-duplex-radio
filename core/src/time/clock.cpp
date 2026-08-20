#include "radio/clock.hpp"

#include <ctime>

namespace radio {
namespace {

// CLOCK_MONOTONIC_RAW exists on macOS 10.12+, on Linux since 2.6.28, and
// therefore on every Android API level this project targets. The fallback is
// kept so a port to an unusual platform degrades to a slewed clock rather than
// failing to build — but it is a real loss of fidelity, so it is spelled out
// rather than hidden.
#if defined(CLOCK_MONOTONIC_RAW)
constexpr clockid_t kClock = CLOCK_MONOTONIC_RAW;
#else
constexpr clockid_t kClock = CLOCK_MONOTONIC;
#endif

}  // namespace

std::uint64_t now_ns() noexcept {
  timespec ts{};
  // clock_gettime on a monotonic clock cannot fail with a valid clock id, so
  // there is no error path worth propagating to callers on the hot path.
  ::clock_gettime(kClock, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ull +
         static_cast<std::uint64_t>(ts.tv_nsec);
}

Micros now_us() noexcept { return now_ns() / 1'000ull; }

}  // namespace radio
