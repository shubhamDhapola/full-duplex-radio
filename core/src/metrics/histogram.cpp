#include "radio/histogram.hpp"

#include <bit>

namespace radio {

std::size_t Histogram::bucket_index(std::uint64_t value) noexcept {
  // Bottom octave is stored linearly, one counter per value, so small
  // latencies are recorded exactly rather than bucketed.
  if (value < kSubBuckets) return static_cast<std::size_t>(value);

  // floor(log2(value)). std::countl_zero is C++20 <bit> and compiles to a
  // single CLZ/LZCNT instruction; the hand-rolled loop version of this is a
  // classic accidental hot spot.
  const auto exponent =
      static_cast<unsigned>(63 - std::countl_zero(value));

  // Which octave above the linear region, and where inside it. Shifting the
  // value down by `octave` keeps the top kPrecisionBits+1 bits, so `within`
  // always lands in [0, kSubBuckets).
  const unsigned octave = exponent - kPrecisionBits;
  const std::uint64_t within = (value >> octave) - kSubBuckets;

  const std::size_t index =
      (static_cast<std::size_t>(octave) + 1) * kSubBuckets +
      static_cast<std::size_t>(within);
  return index < kBucketCount ? index : kBucketCount - 1;
}

std::uint64_t Histogram::bucket_upper_bound(std::size_t index) noexcept {
  if (index < kSubBuckets) return static_cast<std::uint64_t>(index);

  const std::size_t octave = index / kSubBuckets - 1;
  const std::size_t within = index % kSubBuckets;
  const std::uint64_t low =
      static_cast<std::uint64_t>(kSubBuckets + within) << octave;
  return low + (std::uint64_t{1} << octave) - 1;
}

void Histogram::record(std::uint64_t value) noexcept {
  ++buckets_[bucket_index(value)];
  ++count_;
  sum_ += value;
  if (value < min_) min_ = value;
  if (value > max_) max_ = value;
}

void Histogram::reset() noexcept { *this = Histogram{}; }

void Histogram::merge(const Histogram& other) noexcept {
  if (other.count_ == 0) return;
  for (std::size_t i = 0; i < kBucketCount; ++i) buckets_[i] += other.buckets_[i];
  count_ += other.count_;
  sum_ += other.sum_;
  if (other.min_ < min_) min_ = other.min_;
  if (other.max_ > max_) max_ = other.max_;
}

std::uint64_t Histogram::min() const noexcept { return count_ == 0 ? 0 : min_; }

std::uint64_t Histogram::max() const noexcept { return max_; }

double Histogram::mean() const noexcept {
  if (count_ == 0) return 0.0;
  return static_cast<double>(sum_) / static_cast<double>(count_);
}

std::uint64_t Histogram::percentile(double p) const noexcept {
  if (count_ == 0) return 0;
  if (p <= 0.0) return min_;
  if (p >= 100.0) return max_;

  // Rank of the sample we want, 1-based. Rounding to nearest rather than
  // truncating keeps P50 of an even-sized sample from skewing low.
  const double exact_rank = p / 100.0 * static_cast<double>(count_);
  auto rank = static_cast<std::uint64_t>(exact_rank + 0.5);
  if (rank == 0) rank = 1;
  if (rank > count_) rank = count_;

  std::uint64_t cumulative = 0;
  for (std::size_t i = 0; i < kBucketCount; ++i) {
    cumulative += buckets_[i];
    if (cumulative >= rank) {
      const std::uint64_t upper = bucket_upper_bound(i);
      // A bucket's upper bound can exceed anything actually observed, and
      // reporting "P99 = 2047 us" when nothing above 1500 us was ever seen
      // invites exactly the wrong conclusion. Clamp to reality.
      return upper < max_ ? upper : max_;
    }
  }
  return max_;
}

Histogram::Snapshot Histogram::snapshot() const noexcept {
  Snapshot s;
  s.count = count_;
  if (count_ == 0) return s;
  s.min = min_;
  s.max = max_;
  s.mean = mean();
  s.p50 = percentile(50.0);
  s.p95 = percentile(95.0);
  s.p99 = percentile(99.0);
  s.p999 = percentile(99.9);
  return s;
}

}  // namespace radio
