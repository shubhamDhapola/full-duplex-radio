#include "radio/jitter_estimator.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

using namespace radio;
using Catch::Approx;

namespace {

// Feeds a stream: `timestamps` are wire values, `arrivals` local microseconds.
// Arrival times are chosen as multiples of 1000 us throughout these tests so
// that us_to_samples() is exact and the expected values can be hand-computed.
void run(JitterEstimator& j, const std::vector<std::uint32_t>& timestamps,
         const std::vector<Micros>& arrivals) {
  for (std::size_t i = 0; i < timestamps.size(); ++i) {
    j.observe(timestamps[i], arrivals[i]);
  }
}

}  // namespace

TEST_CASE("a fresh estimator has nothing to report") {
  JitterEstimator j;
  CHECK_FALSE(j.has_estimate());
  CHECK(j.samples() == 0);
  CHECK(j.jitter_samples() == 0.0);
}

TEST_CASE("the first packet anchors and contributes no sample") {
  // There is nothing to difference against, so a first packet must not be
  // allowed to invent a delta out of its absolute arrival time.
  JitterEstimator j;
  j.observe(1000, 5'000'000);
  CHECK(j.samples() == 0);
  CHECK(j.jitter_samples() == 0.0);
}

TEST_CASE("perfectly paced arrivals produce zero jitter") {
  JitterEstimator j;
  std::vector<std::uint32_t> ts;
  std::vector<Micros> arr;
  for (std::uint32_t i = 0; i < 50; ++i) {
    ts.push_back(i * kFrameSamples);            // +960 samples per frame
    arr.push_back(100'000 + i * 20'000);        // +20 ms per frame
  }
  run(j, ts, arr);

  CHECK(j.samples() == 49);
  CHECK(j.jitter_samples() == Approx(0.0));
  CHECK(j.peak_abs_delta_samples() == 0);
}

TEST_CASE("a known delay excursion produces the hand-computed estimate") {
  // Frame 3 arrives 5 ms late, then timing recovers.
  //
  //   packet  ts     arrival(us)  arr_delta  ts_delta   D
  //   0       0      100'000      anchor
  //   1       960    120'000      960        960        0
  //   2       1920   145'000      1200       960      +240   (5 ms late)
  //   3       2880   165'000      960        960        0
  //
  //   J after p1: 0 + (0   - 0)/16    = 0
  //   J after p2: 0 + (240 - 0)/16    = 15
  //   J after p3: 15 + (0  - 15)/16   = 14.0625
  JitterEstimator j;
  run(j, {0, 960, 1920, 2880}, {100'000, 120'000, 145'000, 165'000});

  CHECK(j.samples() == 3);
  CHECK(j.jitter_samples() == Approx(14.0625));
  CHECK(j.last_delta_samples() == 0);
  CHECK(j.peak_abs_delta_samples() == 240);

  // 240 samples at 48 kHz is exactly 5 ms — the size of the excursion.
  JitterEstimator k;
  run(k, {0, 960, 1920}, {100'000, 120'000, 145'000});
  CHECK(k.jitter_us() == Approx(15.0 * 1000.0 / 48.0));
}

TEST_CASE("an unsynchronised clock offset does not affect the estimate") {
  // The whole point of the RFC 3550 formulation: the unknown constant offset
  // between two monotonic clocks cancels in the difference, so two receivers
  // whose clocks disagree by 1000 seconds compute identical jitter from
  // identical network behaviour.
  const std::vector<std::uint32_t> ts{0, 960, 1920, 2880, 3840};
  const std::vector<Micros> base{100'000, 122'000, 141'000, 165'000, 183'000};

  JitterEstimator a;
  run(a, ts, base);

  std::vector<Micros> shifted;
  shifted.reserve(base.size());
  for (const auto t : base) shifted.push_back(t + 1'000'000'000ull);

  JitterEstimator b;
  run(b, ts, shifted);

  CHECK(a.jitter_samples() == Approx(b.jitter_samples()));
  CHECK(a.peak_abs_delta_samples() == b.peak_abs_delta_samples());
  CHECK(a.jitter_samples() > 0.0);  // there was real jitter to measure
}

TEST_CASE("early arrivals count as jitter just as much as late ones") {
  // Delay variation is symmetric: a packet arriving 5 ms early disturbs playout
  // exactly as much as one arriving 5 ms late, so the average uses |D|.
  JitterEstimator late;
  run(late, {0, 960}, {100'000, 125'000});  // +5 ms

  JitterEstimator early;
  run(early, {0, 960}, {100'000, 115'000});  // -5 ms

  CHECK(late.jitter_samples() == Approx(early.jitter_samples()));
  CHECK(late.last_delta_samples() == 240);
  CHECK(early.last_delta_samples() == -240);
}

TEST_CASE("timestamp wrap does not corrupt the estimate") {
  // A stream that starts near the 32-bit boundary. Plain subtraction would
  // report a timestamp delta of ~4.3 billion samples (about a day of audio) and
  // the estimate would never recover.
  JitterEstimator j;
  const std::uint32_t start = 0xFFFFFFFFu - 960u;
  run(j, {start, static_cast<std::uint32_t>(start + 960u),
          static_cast<std::uint32_t>(start + 1920u)},
      {100'000, 120'000, 140'000});

  CHECK(j.samples() == 2);
  CHECK(j.jitter_samples() == Approx(0.0));
  CHECK(j.peak_abs_delta_samples() == 0);
}

TEST_CASE("reanchor re-establishes the reference without adding a sample") {
  // At a talkspurt boundary the timestamp jumps by the length of the silence
  // while arrival time jumps by however long the speaker paused. Those are
  // unrelated, so differencing them would inject a spurious spike that then
  // dominates the average for the next ~16 packets — precisely when an adaptive
  // buffer is choosing its depth.
  JitterEstimator j;
  run(j, {0, 960, 1920}, {100'000, 120'000, 140'000});
  const double before = j.jitter_samples();
  const std::uint64_t samples_before = j.samples();

  // 2 seconds of silence: timestamp advances 96'000 samples, wall clock 2.5 s
  // because the speaker hesitated. A raw delta here would be ~24'000 samples.
  j.reanchor(1920 + 96'000, 2'640'000);
  CHECK(j.samples() == samples_before);
  CHECK(j.jitter_samples() == Approx(before));

  // Resumed stream is measured normally against the new anchor.
  j.observe(1920 + 96'960, 2'660'000);
  CHECK(j.samples() == samples_before + 1);
  CHECK(j.last_delta_samples() == 0);
}

TEST_CASE("without reanchoring, a talkspurt gap would poison the estimate") {
  // The negative control for the test above: this is what reanchor() avoids.
  JitterEstimator j;
  run(j, {0, 960, 1920}, {100'000, 120'000, 140'000});
  REQUIRE(j.jitter_samples() == Approx(0.0));

  j.observe(1920 + 96'000, 2'640'000);  // same gap, treated as a normal packet
  CHECK(j.jitter_samples() > 1000.0);   // 1500 samples: 31 ms of phantom jitter
}

TEST_CASE("the peak is retained while the average decays") {
  // J is an exponential average and forgets excursions within ~16 packets. A
  // jitter buffer has to survive the tail, so the peak is tracked separately.
  JitterEstimator j;
  run(j, {0, 960}, {100'000, 100'000 + 20'000});
  j.observe(1920, 140'000 + 60'000);  // one 60 ms excursion
  const std::int64_t peak = j.peak_abs_delta_samples();
  REQUIRE(peak == 2880);              // 60 ms at 48 kHz

  // Forty well-behaved packets later the average has decayed away...
  Micros t = 200'000;
  std::uint32_t ts = 1920;
  for (int i = 0; i < 40; ++i) {
    ts += 960;
    t += 20'000;
    j.observe(ts, t);
  }
  CHECK(j.jitter_samples() < 100.0);
  CHECK(j.peak_abs_delta_samples() == peak);  // ...but the peak is still visible
}

TEST_CASE("reset returns the estimator to its initial state") {
  JitterEstimator j;
  run(j, {0, 960, 1920}, {100'000, 125'000, 140'000});
  REQUIRE(j.has_estimate());

  j.reset();
  CHECK_FALSE(j.has_estimate());
  CHECK(j.samples() == 0);
  CHECK(j.jitter_samples() == 0.0);
  CHECK(j.peak_abs_delta_samples() == 0);

  // Reusable: trackers are recycled per talkspurt.
  j.observe(0, 100'000);
  CHECK(j.samples() == 0);
}
