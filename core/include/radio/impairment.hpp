// A reproducible network impairment model.
#pragma once

#include <cstddef>
#include <cstdint>

#include "radio/clock.hpp"

namespace radio::sim {

// WHY THIS EXISTS RATHER THAN `tc netem`
//
// netem is not reproducible. Two runs with identical settings produce different
// loss patterns, so an A/B comparison -- FEC on versus FEC off at 5% loss --
// measures the change under test plus an unknown amount of pattern noise, and
// there is no way to separate them. A seeded model replays a byte-identical
// impairment sequence, which makes the difference attributable.
//
// It also runs on macOS, which has no tc at all. That is the lesser reason.
//
// WHY BURST LOSS IS MODELLED SEPARATELY FROM INDEPENDENT LOSS
//
// Real Wi-Fi loss is bursty: interference and retry exhaustion drop several
// consecutive frames, not one frame here and there. That distinction decides
// whether a loss-recovery result means anything, because Opus in-band FEC
// reconstructs frame N-1 from frame N's payload -- so it repairs an isolated
// loss almost perfectly and a run of three consecutive losses not at all.
//
// Benchmark only with independent random loss and FEC will look far better than
// it behaves in the field. Hence the Gilbert-Elliott two-state chain below.
enum class JitterShape : std::uint8_t {
  None,
  // Equal probability across [-jitter, +jitter]. Simple, and a poor model of
  // anything real.
  Uniform,
  // Bell-shaped, clamped to +/- 3 sigma. Reasonable for aggregate queueing.
  Normal,
  // Heavy-tailed: mostly small, occasionally enormous. The closest of the three
  // to real wireless delay, and the one that exposes a jitter buffer sized from
  // an average rather than a percentile.
  Pareto,
};

struct Impairment {
  // Applied to every packet. Models propagation plus fixed processing.
  Micros base_delay_us = 0;

  // Magnitude of the variable component. For Normal this is one standard
  // deviation; for Uniform it is the half-width; for Pareto it is the scale.
  Micros jitter_us = 0;
  JitterShape jitter_shape = JitterShape::None;

  // Independent per-packet loss, in percent. Each packet decided in isolation.
  double loss_percent = 0.0;

  // Gilbert-Elliott burst loss. `burst_loss_percent` is the overall long-run
  // loss rate; `burst_mean_length` is the mean number of consecutive packets
  // lost once a burst starts. Setting mean length to 1 makes this equivalent to
  // independent loss, which is a useful control.
  double burst_loss_percent = 0.0;
  double burst_mean_length = 4.0;

  // Duplicated packets, in percent. A duplicate is emitted alongside the
  // original with independently drawn delay, so the two can arrive in either
  // order.
  double duplicate_percent = 0.0;

  // Explicit reordering: this percentage of packets get an extra delay large
  // enough to overtake their successors.
  //
  // Note that jitter alone already causes reordering once its magnitude
  // approaches the packet spacing. This knob exists to produce reordering
  // without also inflating measured jitter, so the two effects can be studied
  // separately.
  double reorder_percent = 0.0;
  Micros reorder_extra_us = 15'000;

  // Token-bucket link limit. 0 means unlimited. Packets arriving with an empty
  // bucket are dropped, which is what a saturated link does when its queue is
  // full.
  std::uint64_t rate_bps = 0;
  std::uint32_t bucket_bytes = 65'536;
};

enum class Verdict : std::uint8_t {
  Forward,
  DropIndependent,
  DropBurst,
  DropRateLimit,
};

[[nodiscard]] const char* to_string(Verdict verdict) noexcept;
[[nodiscard]] const char* to_string(JitterShape shape) noexcept;

struct Decision {
  Verdict verdict = Verdict::Forward;

  // Total delay before release, including base, jitter and any reorder extra.
  Micros delay_us = 0;

  // Emit a second copy. Its own delay is drawn independently, so the duplicate
  // may arrive before or after the original.
  bool duplicate = false;
  Micros duplicate_delay_us = 0;

  bool reorder_applied = false;

  [[nodiscard]] bool forwarded() const noexcept {
    return verdict == Verdict::Forward;
  }
};

// Decides the fate of each packet. Deterministic for a given seed.
//
// The random draws are hand-rolled rather than taken from <random>'s
// distributions, and that is deliberate. std::mt19937_64 is fully specified by
// the standard, so the raw bit stream is portable -- but
// std::uniform_real_distribution, std::normal_distribution and friends are NOT:
// the standard specifies the resulting distribution, not the algorithm that
// produces it. libc++ and libstdc++ consume different numbers of engine
// outputs, so identical seeds give different sequences on macOS and Linux.
//
// For a tool whose entire value is reproducibility -- including across the
// developer's Mac and a CI runner -- that is disqualifying. So the uniform,
// normal (Box-Muller) and Pareto (inverse CDF) transforms are written out here,
// where they are fixed forever.
class ImpairmentEngine {
 public:
  ImpairmentEngine() noexcept = default;
  ImpairmentEngine(const Impairment& config, std::uint64_t seed) noexcept;

  [[nodiscard]] Decision decide(std::size_t packet_bytes, Micros now_us) noexcept;

  // Returns the engine to its initial state, including rewinding the PRNG, so a
  // scenario can be replayed exactly.
  void reset() noexcept;

  [[nodiscard]] const Impairment& config() const noexcept { return config_; }
  [[nodiscard]] std::uint64_t seed() const noexcept { return seed_; }

  // True while the Gilbert-Elliott chain is in its lossy state.
  [[nodiscard]] bool in_burst() const noexcept { return in_burst_; }

  struct Counters {
    std::uint64_t considered = 0;
    std::uint64_t forwarded = 0;
    std::uint64_t dropped_independent = 0;
    std::uint64_t dropped_burst = 0;
    std::uint64_t dropped_rate_limit = 0;
    std::uint64_t duplicated = 0;
    std::uint64_t reordered = 0;
    std::uint64_t bursts_entered = 0;
  };

  [[nodiscard]] const Counters& counters() const noexcept { return counters_; }

  [[nodiscard]] std::uint64_t dropped_total() const noexcept;
  [[nodiscard]] double measured_loss_fraction() const noexcept;

 private:
  [[nodiscard]] std::uint64_t next_bits() noexcept;
  // Uniform in [0, 1).
  [[nodiscard]] double next_unit() noexcept;
  [[nodiscard]] double next_normal() noexcept;
  [[nodiscard]] Micros draw_jitter() noexcept;
  [[nodiscard]] Micros draw_delay() noexcept;
  [[nodiscard]] bool rate_limit_allows(std::size_t bytes, Micros now_us) noexcept;
  void advance_burst_state() noexcept;

  Impairment config_{};
  std::uint64_t seed_ = 0;
  std::uint64_t state_ = 0;

  // Gilbert-Elliott transition probabilities, derived once from the friendlier
  // (overall rate, mean burst length) parameterisation. See the .cpp.
  double p_good_to_bad_ = 0.0;
  double p_bad_to_good_ = 0.0;
  bool in_burst_ = false;

  // Box-Muller produces two normals per pair of uniforms; the spare is kept so
  // the draw count per packet stays predictable.
  double spare_normal_ = 0.0;
  bool has_spare_normal_ = false;

  double bucket_tokens_ = 0.0;
  Micros bucket_updated_us_ = 0;
  bool bucket_started_ = false;

  Counters counters_{};
};

}  // namespace radio::sim
