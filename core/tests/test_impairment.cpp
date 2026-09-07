#include "radio/impairment.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <vector>

using namespace radio;
using namespace radio::sim;
using Catch::Approx;

namespace {

constexpr std::size_t kPacketBytes = 96;  // 80 byte payload + 16 byte header

// Runs `count` packets through the engine at a realistic 50 pps and returns the
// verdicts in order.
std::vector<Verdict> run(ImpairmentEngine& engine, std::size_t count) {
  std::vector<Verdict> verdicts;
  verdicts.reserve(count);
  Micros now = 1'000'000;
  for (std::size_t i = 0; i < count; ++i) {
    verdicts.push_back(engine.decide(kPacketBytes, now).verdict);
    now += 20'000;
  }
  return verdicts;
}

// Lengths of consecutive runs of dropped packets.
std::vector<std::size_t> burst_lengths(const std::vector<Verdict>& verdicts) {
  std::vector<std::size_t> lengths;
  std::size_t current = 0;
  for (const auto verdict : verdicts) {
    if (verdict != Verdict::Forward) {
      ++current;
    } else if (current != 0) {
      lengths.push_back(current);
      current = 0;
    }
  }
  if (current != 0) lengths.push_back(current);
  return lengths;
}

double mean(const std::vector<std::size_t>& values) {
  if (values.empty()) return 0.0;
  double total = 0.0;
  for (const auto value : values) total += static_cast<double>(value);
  return total / static_cast<double>(values.size());
}

}  // namespace

TEST_CASE("a default engine forwards everything untouched") {
  ImpairmentEngine engine(Impairment{}, 1);
  const auto verdicts = run(engine, 1'000);
  CHECK(std::all_of(verdicts.begin(), verdicts.end(),
                    [](Verdict v) { return v == Verdict::Forward; }));
  CHECK(engine.counters().forwarded == 1'000);
  CHECK(engine.dropped_total() == 0);
}

TEST_CASE("the same seed produces a byte-identical impairment pattern") {
  // The property the whole tool exists for. Without this, an A/B comparison
  // measures the change under test plus an unknown amount of pattern noise.
  Impairment config;
  config.loss_percent = 5.0;
  config.base_delay_us = 20'000;
  config.jitter_us = 8'000;
  config.jitter_shape = JitterShape::Normal;
  config.duplicate_percent = 1.0;
  config.reorder_percent = 2.0;

  ImpairmentEngine a(config, 12345);
  ImpairmentEngine b(config, 12345);

  Micros now = 1'000'000;
  for (int i = 0; i < 5'000; ++i) {
    const auto da = a.decide(kPacketBytes, now);
    const auto db = b.decide(kPacketBytes, now);
    REQUIRE(da.verdict == db.verdict);
    REQUIRE(da.delay_us == db.delay_us);
    REQUIRE(da.duplicate == db.duplicate);
    REQUIRE(da.duplicate_delay_us == db.duplicate_delay_us);
    REQUIRE(da.reorder_applied == db.reorder_applied);
    now += 20'000;
  }
}

TEST_CASE("a different seed produces a different pattern") {
  Impairment config;
  config.loss_percent = 10.0;

  ImpairmentEngine a(config, 1);
  ImpairmentEngine b(config, 2);
  CHECK(run(a, 2'000) != run(b, 2'000));
}

TEST_CASE("reset rewinds the generator so a scenario can be replayed") {
  Impairment config;
  config.loss_percent = 7.5;
  config.jitter_us = 5'000;
  config.jitter_shape = JitterShape::Pareto;

  ImpairmentEngine engine(config, 999);
  const auto first = run(engine, 3'000);
  engine.reset();
  const auto second = run(engine, 3'000);

  CHECK(first == second);
  CHECK(engine.counters().considered == 3'000);
}

TEST_CASE("independent loss converges on the requested rate") {
  for (const double target : {1.0, 5.0, 10.0, 25.0}) {
    Impairment config;
    config.loss_percent = target;
    ImpairmentEngine engine(config, 4242);
    run(engine, 200'000);

    const double measured = engine.measured_loss_fraction() * 100.0;
    // 200k samples, so the sampling error is well under a tenth of a percent.
    CHECK(measured == Approx(target).margin(0.3));
    CHECK(engine.counters().dropped_burst == 0);
  }
}

TEST_CASE("independent loss really is independent") {
  // The control case for the burst model: with per-packet decisions the mean
  // run of consecutive losses should be barely above 1.
  Impairment config;
  config.loss_percent = 5.0;
  ImpairmentEngine engine(config, 7);
  const auto lengths = burst_lengths(run(engine, 200'000));

  CHECK(mean(lengths) < 1.2);
  CHECK(mean(lengths) > 1.0);
}

TEST_CASE("burst loss hits the requested rate AND the requested burst length") {
  // Both properties matter. A model that produced 5% loss in the wrong clumping
  // pattern would make FEC results meaningless, since Opus FEC repairs isolated
  // losses well and consecutive ones not at all.
  for (const double length : {2.0, 4.0, 8.0}) {
    Impairment config;
    config.burst_loss_percent = 5.0;
    config.burst_mean_length = length;
    ImpairmentEngine engine(config, 31337);
    const auto verdicts = run(engine, 400'000);

    const double measured_loss = engine.measured_loss_fraction() * 100.0;
    CHECK(measured_loss == Approx(5.0).margin(0.5));

    const auto lengths = burst_lengths(verdicts);
    CHECK(mean(lengths) == Approx(length).margin(length * 0.15));
    CHECK(engine.counters().dropped_independent == 0);
    CHECK(engine.counters().bursts_entered > 100);
  }
}

TEST_CASE("a mean burst length of one degenerates to independent loss") {
  // Useful as an A/B control: same code path, no clumping.
  Impairment config;
  config.burst_loss_percent = 8.0;
  config.burst_mean_length = 1.0;
  ImpairmentEngine engine(config, 55);
  const auto lengths = burst_lengths(run(engine, 200'000));

  CHECK(engine.measured_loss_fraction() * 100.0 == Approx(8.0).margin(0.5));
  CHECK(mean(lengths) < 1.2);
}

TEST_CASE("burst clumping is dramatically different from independent loss") {
  // Same 5% overall loss, wildly different structure. This is the comparison
  // that decides whether an FEC benchmark is honest.
  Impairment independent_config;
  independent_config.loss_percent = 5.0;
  ImpairmentEngine independent(independent_config, 100);
  const auto independent_lengths = burst_lengths(run(independent, 200'000));

  Impairment bursty_config;
  bursty_config.burst_loss_percent = 5.0;
  bursty_config.burst_mean_length = 6.0;
  ImpairmentEngine bursty(bursty_config, 100);
  const auto bursty_lengths = burst_lengths(run(bursty, 200'000));

  // Comparable loss rates...
  CHECK(independent.measured_loss_fraction() ==
        Approx(bursty.measured_loss_fraction()).margin(0.01));
  // ...and a five-fold difference in how the losses arrive.
  CHECK(mean(bursty_lengths) > mean(independent_lengths) * 4.0);
  // Also far fewer, larger outages: the same loss delivered as ~5x fewer
  // events.
  CHECK(bursty_lengths.size() * 4 < independent_lengths.size());
}

TEST_CASE("base delay is applied to every forwarded packet") {
  Impairment config;
  config.base_delay_us = 30'000;
  ImpairmentEngine engine(config, 1);

  for (int i = 0; i < 100; ++i) {
    const auto decision = engine.decide(kPacketBytes, 1'000'000);
    REQUIRE(decision.forwarded());
    CHECK(decision.delay_us == 30'000);
  }
}

TEST_CASE("uniform jitter stays centred on the base delay") {
  Impairment config;
  config.base_delay_us = 40'000;
  config.jitter_us = 10'000;
  config.jitter_shape = JitterShape::Uniform;
  ImpairmentEngine engine(config, 8);

  Micros minimum = ~Micros{0};
  Micros maximum = 0;
  double total = 0.0;
  constexpr int kSamples = 50'000;

  for (int i = 0; i < kSamples; ++i) {
    const auto decision = engine.decide(kPacketBytes, 1'000'000);
    REQUIRE(decision.forwarded());
    minimum = std::min(minimum, decision.delay_us);
    maximum = std::max(maximum, decision.delay_us);
    total += static_cast<double>(decision.delay_us);
  }

  CHECK(total / kSamples == Approx(40'000.0).margin(200.0));
  CHECK(minimum >= 30'000);
  CHECK(maximum <= 50'000);
  CHECK(minimum < 31'000);  // the full range is actually explored
  CHECK(maximum > 49'000);
}

TEST_CASE("pareto jitter is one-sided and heavy-tailed") {
  // The distinguishing property: a queue can delay a packet but never deliver
  // it early, and the tail is what breaks a buffer sized from the mean.
  Impairment config;
  config.base_delay_us = 10'000;
  config.jitter_us = 2'000;
  config.jitter_shape = JitterShape::Pareto;
  ImpairmentEngine engine(config, 77);

  std::vector<Micros> delays;
  delays.reserve(50'000);
  for (int i = 0; i < 50'000; ++i) {
    const auto decision = engine.decide(kPacketBytes, 1'000'000);
    REQUIRE(decision.forwarded());
    CHECK(decision.delay_us >= 10'000);  // never early
    delays.push_back(decision.delay_us);
  }

  std::sort(delays.begin(), delays.end());
  const auto median = delays[delays.size() / 2];
  const auto p999 = delays[static_cast<std::size_t>(
      static_cast<double>(delays.size()) * 0.999)];

  // Heavy tail: the 99.9th percentile is many times the median excess. A normal
  // distribution clamped at 3 sigma could not do this.
  CHECK(p999 - 10'000 > (median - 10'000) * 10);
}

TEST_CASE("normal jitter is clamped so one freak sample cannot ruin a run") {
  Impairment config;
  config.base_delay_us = 50'000;
  config.jitter_us = 5'000;
  config.jitter_shape = JitterShape::Normal;
  ImpairmentEngine engine(config, 21);

  for (int i = 0; i < 100'000; ++i) {
    const auto decision = engine.decide(kPacketBytes, 1'000'000);
    REQUIRE(decision.forwarded());
    // +/- 3 sigma of 5 ms around a 50 ms base.
    CHECK(decision.delay_us >= 35'000);
    CHECK(decision.delay_us <= 65'000);
  }
}

TEST_CASE("delay never goes negative when jitter exceeds the base delay") {
  // Physically meaningless, and on an unsigned type it would become an enormous
  // positive delay -- the same trap as everywhere else in this codebase.
  Impairment config;
  config.base_delay_us = 1'000;
  config.jitter_us = 20'000;
  config.jitter_shape = JitterShape::Uniform;
  ImpairmentEngine engine(config, 3);

  for (int i = 0; i < 50'000; ++i) {
    const auto decision = engine.decide(kPacketBytes, 1'000'000);
    CHECK(decision.delay_us < 1'000'000);  // not a wrapped value
  }
}

TEST_CASE("duplication emits a copy with an independently drawn delay") {
  Impairment config;
  config.duplicate_percent = 100.0;
  config.base_delay_us = 20'000;
  config.jitter_us = 10'000;
  config.jitter_shape = JitterShape::Uniform;
  ImpairmentEngine engine(config, 5);

  bool saw_copy_first = false;
  bool saw_original_first = false;
  for (int i = 0; i < 1'000; ++i) {
    const auto decision = engine.decide(kPacketBytes, 1'000'000);
    REQUIRE(decision.duplicate);
    if (decision.duplicate_delay_us < decision.delay_us) saw_copy_first = true;
    if (decision.duplicate_delay_us > decision.delay_us)
      saw_original_first = true;
  }
  // A real duplicating link produces both orders, so the model must too.
  CHECK(saw_copy_first);
  CHECK(saw_original_first);
  CHECK(engine.counters().duplicated == 1'000);
}

TEST_CASE("reordering adds enough delay to overtake later packets") {
  Impairment config;
  config.reorder_percent = 100.0;
  config.reorder_extra_us = 15'000;
  ImpairmentEngine engine(config, 6);

  const auto decision = engine.decide(kPacketBytes, 1'000'000);
  CHECK(decision.reorder_applied);
  // Larger than the 20 ms frame spacing would need to be for a swap... this one
  // is 15 ms, so it overtakes at most one packet. Deliberately modest: the knob
  // is for producing reordering, not for producing chaos.
  CHECK(decision.delay_us == 15'000);
  CHECK(engine.counters().reordered == 1);
}

TEST_CASE("reordering can be produced without inflating measured jitter") {
  // The reason this is a separate knob from jitter: a scenario can study
  // reordering in isolation.
  Impairment config;
  config.reorder_percent = 3.0;
  ImpairmentEngine engine(config, 11);
  run(engine, 100'000);

  const double fraction = static_cast<double>(engine.counters().reordered) /
                          static_cast<double>(engine.counters().forwarded);
  CHECK(fraction * 100.0 == Approx(3.0).margin(0.3));
  CHECK(engine.dropped_total() == 0);
}

TEST_CASE("a token bucket enforces its rate over the long run") {
  Impairment config;
  config.rate_bps = 32'000;  // about one voice stream
  config.bucket_bytes = 4'000;
  ImpairmentEngine engine(config, 9);

  // Offer double the link rate: 100 pps of 96-byte packets is ~77 kbps.
  Micros now = 1'000'000;
  std::uint64_t bytes_passed = 0;
  constexpr int kPackets = 20'000;
  for (int i = 0; i < kPackets; ++i) {
    if (engine.decide(kPacketBytes, now).forwarded()) {
      bytes_passed += kPacketBytes;
    }
    now += 10'000;  // 100 pps
  }

  const double seconds = static_cast<double>(kPackets) * 0.01;
  const double achieved_bps = static_cast<double>(bytes_passed) * 8.0 / seconds;
  CHECK(achieved_bps <= 32'000.0 * 1.05);
  CHECK(achieved_bps > 32'000.0 * 0.9);
  CHECK(engine.counters().dropped_rate_limit > 0);
}

TEST_CASE("an idle link accumulates only a bounded burst allowance") {
  // The bucket cap is what stops an idle link granting unlimited credit.
  Impairment config;
  config.rate_bps = 8'000;
  config.bucket_bytes = 500;
  ImpairmentEngine engine(config, 10);

  // Idle for an hour, then offer a flood.
  Micros now = 3'600'000'000;
  std::size_t passed = 0;
  for (int i = 0; i < 100; ++i) {
    if (engine.decide(100, now).forwarded()) ++passed;
  }
  CHECK(passed <= 5);  // 500 byte bucket / 100 byte packets
  CHECK(passed >= 1);
}

TEST_CASE("verdict and shape names are distinct for the ground-truth log") {
  CHECK(std::string_view{to_string(Verdict::Forward)} == "forward");
  CHECK(std::string_view{to_string(Verdict::DropIndependent)} ==
        "drop_independent");
  CHECK(std::string_view{to_string(Verdict::DropBurst)} == "drop_burst");
  CHECK(std::string_view{to_string(Verdict::DropRateLimit)} ==
        "drop_rate_limit");
  CHECK(std::string_view{to_string(JitterShape::Pareto)} == "pareto");
}
