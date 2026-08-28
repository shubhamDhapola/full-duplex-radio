#include "radio/impairment.hpp"

#include <cmath>

namespace radio::sim {
namespace {

// splitmix64. Used to spread a user-supplied seed -- "--seed 1" and "--seed 2"
// differ in a single bit, and feeding those straight into a generator gives
// correlated early output. One mixing step removes that.
std::uint64_t splitmix64(std::uint64_t& state) noexcept {
  state += 0x9E3779B97F4A7C15ull;
  std::uint64_t z = state;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

constexpr double kPi = 3.14159265358979323846;

// Pareto tail index. 1.5 gives a distribution with a finite mean and an infinite
// variance, which is a fair caricature of wireless delay: usually small, and
// occasionally very large indeed.
constexpr double kParetoAlpha = 1.5;

// Normal draws are clamped so a single freak sample cannot produce a delay of
// several seconds and make a whole run unreadable.
constexpr double kNormalClampSigma = 3.0;

double clamp_percent(double value) noexcept {
  if (value < 0.0) return 0.0;
  if (value > 100.0) return 100.0;
  return value;
}

}  // namespace

const char* to_string(Verdict verdict) noexcept {
  switch (verdict) {
    case Verdict::Forward:
      return "forward";
    case Verdict::DropIndependent:
      return "drop_independent";
    case Verdict::DropBurst:
      return "drop_burst";
    case Verdict::DropRateLimit:
      return "drop_rate_limit";
  }
  return "invalid";
}

const char* to_string(JitterShape shape) noexcept {
  switch (shape) {
    case JitterShape::None:
      return "none";
    case JitterShape::Uniform:
      return "uniform";
    case JitterShape::Normal:
      return "normal";
    case JitterShape::Pareto:
      return "pareto";
  }
  return "invalid";
}

ImpairmentEngine::ImpairmentEngine(const Impairment& config,
                                   std::uint64_t seed) noexcept
    : config_(config), seed_(seed) {
  config_.loss_percent = clamp_percent(config_.loss_percent);
  config_.burst_loss_percent = clamp_percent(config_.burst_loss_percent);
  config_.duplicate_percent = clamp_percent(config_.duplicate_percent);
  config_.reorder_percent = clamp_percent(config_.reorder_percent);
  if (config_.burst_mean_length < 1.0) config_.burst_mean_length = 1.0;

  // Gilbert-Elliott, derived from parameters a human can reason about.
  //
  // The chain has two states. In GOOD nothing is lost; in BAD everything is.
  // Writing L for the overall loss fraction and B for the mean burst length:
  //
  //   p(bad -> good) = 1 / B                 a burst lasts B packets on average
  //   P(BAD)         = L                     loss only happens in BAD
  //
  // and the steady state of a two-state chain gives
  //
  //   P(BAD) = p_gb / (p_gb + p_bg)
  //
  // Solving for p_gb:
  //
  //   p_gb = L * p_bg / (1 - L)
  //
  // So 5% loss in bursts averaging 4 packets means p_bg = 0.25 and
  // p_gb = 0.0132: a good run of about 76 packets, then 4 lost. Far closer to
  // what a Wi-Fi capture looks like than 5% scattered independently.
  const double loss_fraction = config_.burst_loss_percent / 100.0;
  if (loss_fraction > 0.0 && loss_fraction < 1.0) {
    p_bad_to_good_ = 1.0 / config_.burst_mean_length;
    p_good_to_bad_ = loss_fraction * p_bad_to_good_ / (1.0 - loss_fraction);
  } else if (loss_fraction >= 1.0) {
    p_good_to_bad_ = 1.0;
    p_bad_to_good_ = 0.0;
  }

  reset();
}

void ImpairmentEngine::reset() noexcept {
  state_ = seed_;
  in_burst_ = false;
  has_spare_normal_ = false;
  spare_normal_ = 0.0;
  bucket_tokens_ = static_cast<double>(config_.bucket_bytes);
  bucket_updated_us_ = 0;
  bucket_started_ = false;
  counters_ = Counters{};
}

std::uint64_t ImpairmentEngine::next_bits() noexcept {
  return splitmix64(state_);
}

double ImpairmentEngine::next_unit() noexcept {
  // The standard idiom: take the top 53 bits, which is exactly the mantissa
  // width of a double, and scale. Every representable value in [0, 1) is
  // reachable and none is favoured. Writing `bits % 1000000 / 1000000.0` would
  // introduce modulo bias.
  return static_cast<double>(next_bits() >> 11) * 0x1.0p-53;
}

double ImpairmentEngine::next_normal() noexcept {
  if (has_spare_normal_) {
    has_spare_normal_ = false;
    return spare_normal_;
  }
  // Box-Muller. std::normal_distribution would be shorter but is not portably
  // reproducible -- see the note in the header.
  double u1 = next_unit();
  const double u2 = next_unit();
  // log(0) is -inf, so nudge a zero draw to the smallest positive double we can
  // reach from this generator.
  if (u1 <= 0.0) u1 = 0x1.0p-53;
  const double magnitude = std::sqrt(-2.0 * std::log(u1));
  const double angle = 2.0 * kPi * u2;
  spare_normal_ = magnitude * std::sin(angle);
  has_spare_normal_ = true;
  return magnitude * std::cos(angle);
}

Micros ImpairmentEngine::draw_jitter() noexcept {
  if (config_.jitter_us == 0 || config_.jitter_shape == JitterShape::None) {
    return 0;
  }
  const auto scale = static_cast<double>(config_.jitter_us);

  double offset = 0.0;
  switch (config_.jitter_shape) {
    case JitterShape::Uniform:
      // Symmetric about zero, so mean delay stays at base_delay.
      offset = (next_unit() * 2.0 - 1.0) * scale;
      break;
    case JitterShape::Normal: {
      double sigma = next_normal();
      if (sigma > kNormalClampSigma) sigma = kNormalClampSigma;
      if (sigma < -kNormalClampSigma) sigma = -kNormalClampSigma;
      offset = sigma * scale;
      break;
    }
    case JitterShape::Pareto: {
      // Inverse CDF: x = scale / U^(1/alpha), which is always >= scale, so this
      // one is one-sided. Delay is only ever added -- which is correct, since a
      // queue cannot deliver a packet early.
      double u = next_unit();
      if (u <= 0.0) u = 0x1.0p-53;
      offset = scale / std::pow(u, 1.0 / kParetoAlpha) - scale;
      break;
    }
    case JitterShape::None:
      break;
  }

  const double total = static_cast<double>(config_.base_delay_us) + offset;
  // A negative total delay is meaningless; clamping to zero is the honest
  // outcome and it is why symmetric jitter larger than base_delay quietly
  // becomes asymmetric.
  if (total <= 0.0) return 0;
  return static_cast<Micros>(total) - config_.base_delay_us;
}

Micros ImpairmentEngine::draw_delay() noexcept {
  return config_.base_delay_us + draw_jitter();
}

bool ImpairmentEngine::rate_limit_allows(std::size_t bytes,
                                        Micros now_us) noexcept {
  if (config_.rate_bps == 0) return true;

  if (!bucket_started_) {
    bucket_updated_us_ = now_us;
    bucket_started_ = true;
  }

  // Refill proportional to elapsed time, capped at the bucket size. The cap is
  // what makes this a *burst* allowance rather than unlimited credit for an idle
  // link.
  const auto elapsed = now_us > bucket_updated_us_ ? now_us - bucket_updated_us_ : 0;
  bucket_updated_us_ = now_us;
  const double bytes_per_us = static_cast<double>(config_.rate_bps) / 8.0 / 1e6;
  bucket_tokens_ += static_cast<double>(elapsed) * bytes_per_us;
  const auto capacity = static_cast<double>(config_.bucket_bytes);
  if (bucket_tokens_ > capacity) bucket_tokens_ = capacity;

  const auto cost = static_cast<double>(bytes);
  if (bucket_tokens_ < cost) return false;
  bucket_tokens_ -= cost;
  return true;
}

void ImpairmentEngine::advance_burst_state() noexcept {
  if (p_good_to_bad_ <= 0.0 && p_bad_to_good_ <= 0.0) return;

  if (in_burst_) {
    if (next_unit() < p_bad_to_good_) in_burst_ = false;
  } else {
    if (next_unit() < p_good_to_bad_) {
      in_burst_ = true;
      ++counters_.bursts_entered;
    }
  }
}

Decision ImpairmentEngine::decide(std::size_t packet_bytes,
                                  Micros now_us) noexcept {
  ++counters_.considered;
  Decision decision;

  // Order matters and is fixed so a scenario replays identically: the burst
  // chain advances for every packet considered, whether or not the packet
  // survives. Advancing it only for survivors would make the chain's behaviour
  // depend on the independent-loss draw, and the two impairments would stop
  // being separable.
  advance_burst_state();

  if (in_burst_) {
    decision.verdict = Verdict::DropBurst;
    ++counters_.dropped_burst;
    return decision;
  }

  if (config_.loss_percent > 0.0 &&
      next_unit() * 100.0 < config_.loss_percent) {
    decision.verdict = Verdict::DropIndependent;
    ++counters_.dropped_independent;
    return decision;
  }

  if (!rate_limit_allows(packet_bytes, now_us)) {
    decision.verdict = Verdict::DropRateLimit;
    ++counters_.dropped_rate_limit;
    return decision;
  }

  decision.delay_us = draw_delay();

  if (config_.reorder_percent > 0.0 &&
      next_unit() * 100.0 < config_.reorder_percent) {
    decision.delay_us += config_.reorder_extra_us;
    decision.reorder_applied = true;
    ++counters_.reordered;
  }

  if (config_.duplicate_percent > 0.0 &&
      next_unit() * 100.0 < config_.duplicate_percent) {
    decision.duplicate = true;
    // Independent draw, so the copy can land before or after the original --
    // which is what a duplicating link actually does.
    decision.duplicate_delay_us = draw_delay();
    ++counters_.duplicated;
  }

  ++counters_.forwarded;
  return decision;
}

std::uint64_t ImpairmentEngine::dropped_total() const noexcept {
  return counters_.dropped_independent + counters_.dropped_burst +
         counters_.dropped_rate_limit;
}

double ImpairmentEngine::measured_loss_fraction() const noexcept {
  if (counters_.considered == 0) return 0.0;
  return static_cast<double>(dropped_total()) /
         static_cast<double>(counters_.considered);
}

}  // namespace radio::sim
