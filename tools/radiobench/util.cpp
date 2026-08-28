#include "util.hpp"

#include <csignal>
#include <ctime>

namespace radiobench {
namespace {

// `volatile sig_atomic_t` is the only type the C++ standard permits a signal
// handler to write. std::atomic<bool> would look more modern and is not
// guaranteed to be async-signal-safe.
volatile std::sig_atomic_t g_stop = 0;

void on_signal(int) { g_stop = 1; }

}  // namespace

void sleep_us(radio::Micros duration) {
  if (duration == 0) return;
  timespec request{};
  request.tv_sec = static_cast<time_t>(duration / 1'000'000ull);
  request.tv_nsec = static_cast<long>((duration % 1'000'000ull) * 1'000ull);
  // A signal can cut the sleep short. We deliberately do not resume: the caller
  // is either shutting down (so returning early is correct) or driven by a
  // pacer that will recompute the remaining delay anyway.
  ::nanosleep(&request, nullptr);
}

void install_signal_handlers() {
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
}

bool stop_requested() { return g_stop != 0; }

}  // namespace radiobench
