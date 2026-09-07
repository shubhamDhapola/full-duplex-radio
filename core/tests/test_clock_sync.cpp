#include "radio/clock_sync.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

using namespace radio;
using Catch::Approx;

namespace {

// A simulated link with a known offset and known one-way delays, so the
// estimator can be checked against ground truth rather than against itself.
struct Probe {
  Micros t1 = 0;
  Micros t2 = 0;
  Micros t3 = 0;
  Micros t4 = 0;
};

// `offset` is the peer's clock minus ours. `d1` is our->peer delay, `d2` the
// return leg, `think` the time the peer spends between receiving and replying.
Probe simulate(Micros send_at, std::int64_t offset, std::int64_t d1,
               std::int64_t d2, std::int64_t think = 0) {
  Probe p;
  p.t1 = send_at;
  p.t2 = static_cast<Micros>(static_cast<std::int64_t>(p.t1) + d1 + offset);
  p.t3 = static_cast<Micros>(static_cast<std::int64_t>(p.t2) + think);
  p.t4 = static_cast<Micros>(static_cast<std::int64_t>(p.t3) + d2 - offset);
  return p;
}

ClockSync::Sample feed(ClockSync& sync, const Probe& p) {
  return sync.observe(p.t1, p.t2, p.t3, p.t4);
}

// A realistic starting point on a monotonic clock: about 11.5 days of uptime.
constexpr Micros kBase = 1'000'000'000'000ull;

}  // namespace

TEST_CASE("a fresh estimator has no opinion") {
  ClockSync sync;
  CHECK_FALSE(sync.has_estimate());
  CHECK(sync.offset_us() == 0);
  CHECK(sync.rtt_us() == 0);
  CHECK(sync.offset_uncertainty_us() == 0);
  CHECK_FALSE(sync.best().valid);

  // With no estimate, translation is the identity rather than a guess.
  CHECK(sync.remote_to_local(12'345) == 12'345);
}

TEST_CASE("a symmetric path recovers the offset exactly") {
  ClockSync sync;
  const std::int64_t true_offset = 4'200'000;  // peer is 4.2 s ahead
  const auto sample = feed(sync, simulate(kBase, true_offset, 500, 500));

  REQUIRE(sample.valid);
  CHECK(sample.rtt_us == 1'000);
  CHECK(sample.offset_us == true_offset);
  CHECK(sync.has_estimate());
}

TEST_CASE("RTT is exact no matter how far apart the clocks are") {
  // The offset cancels when the two legs are added, so RTT needs no
  // synchronisation at all -- the same algebraic cancellation the jitter
  // estimator relies on.
  for (const std::int64_t offset : {std::int64_t{0}, std::int64_t{-999'000'000},
                                    std::int64_t{500'000'000'000}}) {
    ClockSync sync;
    const auto sample = feed(sync, simulate(kBase, offset, 3'000, 3'000));
    REQUIRE(sample.valid);
    CHECK(sample.rtt_us == 6'000);
  }
}

TEST_CASE("RTT excludes the time the peer spent thinking") {
  // (t3 - t2) is subtracted, so this measures the network rather than the
  // peer's responsiveness. A slow peer must not look like a slow link.
  ClockSync sync;
  const auto sample = feed(sync, simulate(kBase, 0, 2'000, 2'000, 50'000));
  REQUIRE(sample.valid);
  CHECK(sample.rtt_us == 4'000);
}

TEST_CASE("path asymmetry biases the offset by exactly half the difference") {
  // The estimator assumes d1 == d2. When that is false the error is
  // (d1 - d2) / 2, and nothing in the four timestamps can reveal it. This test
  // pins down the size of the blind spot.
  ClockSync sync;
  const std::int64_t true_offset = 1'000;
  const std::int64_t d1 = 9'000;
  const std::int64_t d2 = 1'000;

  const auto sample = feed(sync, simulate(kBase, true_offset, d1, d2));
  REQUIRE(sample.valid);
  CHECK(sample.rtt_us == 10'000);
  CHECK(sample.offset_us == true_offset + (d1 - d2) / 2);

  // ...and the reported uncertainty is large enough to cover it.
  const std::int64_t error = sample.offset_us - true_offset;
  CHECK(error <= sync.offset_uncertainty_us());
  CHECK(sync.offset_uncertainty_us() == 5'000);
}

TEST_CASE("the uncertainty is half the RTT of the selected sample") {
  ClockSync sync;
  feed(sync, simulate(kBase, 0, 20'000, 20'000));  // rtt 40 ms
  CHECK(sync.offset_uncertainty_us() == 20'000);

  // A quieter probe arrives: the estimate improves and so does its error bar.
  feed(sync, simulate(kBase + 1'000'000, 0, 1'000, 1'000));  // rtt 2 ms
  CHECK(sync.rtt_us() == 2'000);
  CHECK(sync.offset_uncertainty_us() == 1'000);
}

TEST_CASE("selecting the lowest-RTT sample beats averaging all of them") {
  // The central design decision. Queueing is one-sided: here the outbound leg
  // is congested, so every queued sample's offset is biased the same way.
  // Averaging embeds that bias; taking the minimum-RTT sample avoids it.
  ClockSync sync;
  const std::int64_t true_offset = 5'000;

  Micros when = kBase;
  std::int64_t sum_of_offsets = 0;
  int count = 0;

  // Seven congested probes: 20 ms of queueing outbound, none on the return.
  for (int i = 0; i < 7; ++i) {
    const auto sample = feed(sync, simulate(when, true_offset, 20'500, 500));
    REQUIRE(sample.valid);
    sum_of_offsets += sample.offset_us;
    ++count;
    when += 1'000'000;
  }

  // One quiet moment on the path.
  const auto quiet = feed(sync, simulate(when, true_offset, 500, 500));
  REQUIRE(quiet.valid);
  sum_of_offsets += quiet.offset_us;
  ++count;

  // Selection finds the good sample and the offset is exact.
  CHECK(sync.rtt_us() == 1'000);
  CHECK(sync.offset_us() == true_offset);

  // Averaging would have been out by 8.75 ms -- and, crucially, averaging more
  // samples would not have helped, because the error is a bias and not noise.
  const std::int64_t averaged = sum_of_offsets / count;
  CHECK(averaged != true_offset);
  CHECK(averaged - true_offset > 8'000);
}

TEST_CASE("the window keeps only the most recent samples") {
  // A very good sample that has aged out must stop being used: clock drift
  // means an old offset is stale even if it was well measured.
  ClockSync sync;
  Micros when = kBase;

  feed(sync, simulate(when, 0, 100, 100));  // rtt 200, excellent
  when += 1'000'000;

  for (std::size_t i = 0; i < ClockSync::kWindowSize; ++i) {
    feed(sync, simulate(when, 0, 5'000, 5'000));  // rtt 10'000
    when += 1'000'000;
  }

  CHECK(sync.accepted() == ClockSync::kWindowSize + 1);
  CHECK(sync.rtt_us() == 10'000);  // the excellent sample has been evicted
}

TEST_CASE("self-inconsistent exchanges are rejected, not absorbed") {
  ClockSync sync;

  // Our clock appears to run backwards across the exchange.
  CHECK_FALSE(sync.observe(kBase + 1'000, kBase, kBase, kBase).valid);

  // The peer's clock appears to run backwards.
  CHECK_FALSE(
      sync.observe(kBase, kBase + 5'000, kBase + 4'000, kBase + 10'000).valid);

  // The peer claims to have spent longer thinking than the whole exchange took,
  // which would make RTT negative.
  CHECK_FALSE(sync.observe(kBase, kBase, kBase + 50'000, kBase + 10'000).valid);

  CHECK(sync.rejected() == 3);
  CHECK(sync.accepted() == 0);
  CHECK_FALSE(sync.has_estimate());
}

TEST_CASE("an implausibly slow exchange is rejected") {
  // On a LAN a multi-second RTT means a stale PONG was matched to the wrong
  // probe, not a very bad path. Accepting it would poison the window.
  ClockSync sync;
  const auto sample =
      feed(sync, simulate(kBase, 0, 3'000'000, 3'000'000));  // 6 s
  CHECK_FALSE(sample.valid);
  CHECK(sync.rejected() == 1);
}

TEST_CASE("timestamps translate between clock domains and back") {
  ClockSync sync;
  const std::int64_t true_offset = -750'000;  // peer is behind us
  feed(sync, simulate(kBase, true_offset, 400, 400));

  const Micros local = kBase + 12'345;
  const Micros remote = sync.local_to_remote(local);
  CHECK(static_cast<std::int64_t>(remote) ==
        static_cast<std::int64_t>(local) + true_offset);
  CHECK(sync.remote_to_local(remote) == local);
}

TEST_CASE("a drifting peer clock is detected and quantified") {
  // The peer's oscillator runs 20 ppm fast, so the offset grows by 20 us every
  // second. Over an hour that is 72 ms of accumulated buffer error, which is
  // why M4 has to correct for it.
  ClockSync sync;
  constexpr std::int64_t kPpm = 20;

  Micros when = kBase;
  for (int k = 0; k < 39; ++k) {
    const std::int64_t offset = 5'000 + kPpm * k;  // +20 us per second
    feed(sync, simulate(when, offset, 500, 500));
    when += 1'000'000;
  }
  // A final, quieter probe so the selected sample is unambiguous.
  feed(sync, simulate(when, 5'000 + kPpm * 39, 400, 400));

  REQUIRE(sync.has_drift_estimate());
  CHECK(sync.drift_ppm() == Approx(static_cast<double>(kPpm)).margin(0.5));
}

TEST_CASE(
    "drift is withheld until the baseline is long enough to mean anything") {
  // Each offset carries +/- rtt/2 of error, so a short baseline makes the slope
  // mostly noise. Reporting a number there would be worse than reporting none.
  ClockSync sync;
  Micros when = kBase;
  for (std::size_t i = 0; i < ClockSync::kWindowSize; ++i) {
    feed(sync, simulate(when, 5'000, 500, 500));
    when += 100'000;  // only 100 ms apart
  }
  CHECK(sync.accepted() == ClockSync::kWindowSize);
  CHECK_FALSE(sync.has_drift_estimate());
  CHECK(sync.drift_ppm() == 0.0);
}

TEST_CASE("a stable pair of clocks reports no drift") {
  ClockSync sync;
  Micros when = kBase;
  for (int k = 0; k < 40; ++k) {
    feed(sync, simulate(when, 5'000, 500, 500));
    when += 1'000'000;
  }
  REQUIRE(sync.has_drift_estimate());
  CHECK(sync.drift_ppm() == Approx(0.0).margin(0.1));
}

TEST_CASE("RTT percentiles are available, not just the best sample") {
  // The selected sample answers "what is the offset". The distribution answers
  // "what is this path like", and a mean would hide the tail.
  ClockSync sync;
  Micros when = kBase;
  for (int i = 0; i < 100; ++i) {
    const std::int64_t leg = (i == 99) ? 40'000 : 1'000;  // one bad probe
    feed(sync, simulate(when, 0, leg, leg));
    when += 1'000'000;
  }

  const auto snapshot = sync.rtt_histogram().snapshot();
  CHECK(snapshot.count == 100);
  CHECK(snapshot.min == 2'000);
  CHECK(snapshot.max == 80'000);
  CHECK(snapshot.p50 < 3'000);
  CHECK(snapshot.p999 >= 40'000);
}

TEST_CASE("reset clears every accumulated estimate") {
  ClockSync sync;
  Micros when = kBase;
  for (int k = 0; k < 20; ++k) {
    feed(sync, simulate(when, 5'000, 500, 500));
    when += 1'000'000;
  }
  REQUIRE(sync.has_estimate());

  sync.reset();
  CHECK_FALSE(sync.has_estimate());
  CHECK(sync.accepted() == 0);
  CHECK(sync.rejected() == 0);
  CHECK_FALSE(sync.has_drift_estimate());
  CHECK(sync.rtt_histogram().empty());

  feed(sync, simulate(kBase, 100, 500, 500));
  CHECK(sync.offset_us() == 100);
}
