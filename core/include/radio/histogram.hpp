// Latency histogram with bounded memory and a guaranteed precision bound.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace radio {

// Records values (microseconds, almost always) and answers percentile queries.
//
// WHY A HISTOGRAM AND NOT A MEAN
//
// Every number that matters in this project lives in the tail. A mean latency of
// 45 ms tells you nothing about whether the call sounded good; a P99 of 210 ms
// tells you 1 packet in 100 arrived too late to play. Averages actively mislead
// here, because one 400 ms excursion in a thousand packets moves the mean by
// 0.4 ms and is invisible — while being exactly the event the user heard.
//
// This is also why the jitter estimator's exponential average is not enough on
// its own. A stream with J = 3 ms and an occasional 60 ms excursion looks
// perfectly healthy on the average and underruns repeatedly in practice. Sizing
// a buffer from the mean is the standard way to build something that passes
// testing and glitches in the field.
//
// WHY NOT JUST KEEP EVERY SAMPLE AND SORT
//
// Exact percentiles need the full sample set. For a benchmark run that is
// affordable, but the same code has to run on the phone during a call, for
// hours, per peer, per pipeline stage. Storage would grow without bound, and
// the recording path would allocate — which is forbidden (lesson 01 §6).
//
// WHY NOT FIXED-WIDTH BUCKETS
//
// 1 microsecond buckets covering 0..1 second is a million counters, 8 MB. And
// the resolution is wrong at both ends: absurdly fine at 10 us, uselessly coarse
// if a value ever reaches 10 seconds.
//
// THE SCHEME USED HERE
//
// Log-linear bucketing, the same idea as HdrHistogram. Split the value range
// into octaves (powers of two), and divide each octave into a fixed number of
// equal-width sub-buckets. Bucket *width* then scales with magnitude, so
// *relative* precision is constant:
//
//     value range      bucket width     relative error
//     32..63           1                <= 3.1%
//     64..127          2                <= 3.1%
//     1024..2047       32               <= 3.1%
//     1e6..2e6         32768            <= 3.1%
//
// With 5 precision bits that is 32 sub-buckets per octave and a worst-case
// relative error of 1/32 = 3.13%. For latency reporting that is the right
// trade: nobody needs to know whether P99 was 143.2 ms or 143.6 ms, but the
// difference between 143 ms and 210 ms matters enormously.
//
// Cost: 1920 counters, 15 KB, fixed forever. Recording is one count-leading-
// zeros, one shift, one increment — no branches worth worrying about, no
// allocation, safe on any thread that owns the object.
//
// Count, min, max and sum are tracked exactly. Only percentiles are
// approximate, and their error is bounded and documented rather than unknown.
class Histogram {
 public:
  static constexpr unsigned kPrecisionBits = 5;
  static constexpr std::size_t kSubBuckets = std::size_t{1} << kPrecisionBits;

  // Octaves for a 64-bit value, plus the linear region at the bottom.
  static constexpr std::size_t kBucketCount =
      (64 - kPrecisionBits + 1) * kSubBuckets;

  // Worst-case relative error of any percentile answer.
  static constexpr double kRelativeError = 1.0 / static_cast<double>(kSubBuckets);

  void record(std::uint64_t value) noexcept;
  void reset() noexcept;

  // Combine another histogram into this one. Used to fold per-thread or
  // per-peer histograms together without a lock on the recording path.
  void merge(const Histogram& other) noexcept;

  [[nodiscard]] std::uint64_t count() const noexcept { return count_; }
  [[nodiscard]] bool empty() const noexcept { return count_ == 0; }

  // Exact, not bucketed.
  [[nodiscard]] std::uint64_t min() const noexcept;
  [[nodiscard]] std::uint64_t max() const noexcept;
  [[nodiscard]] std::uint64_t total() const noexcept { return sum_; }
  [[nodiscard]] double mean() const noexcept;

  // `p` in [0, 100]. Returns the upper bound of the bucket containing the
  // requested rank, clamped to the largest value actually recorded.
  //
  // Reporting the upper bound rather than the midpoint is deliberate: it never
  // understates latency. A latency figure that errs optimistically is worse
  // than useless, because it is the number someone will quote.
  [[nodiscard]] std::uint64_t percentile(double p) const noexcept;

  struct Snapshot {
    std::uint64_t count = 0;
    std::uint64_t min = 0;
    std::uint64_t max = 0;
    double mean = 0.0;
    std::uint64_t p50 = 0;
    std::uint64_t p95 = 0;
    std::uint64_t p99 = 0;
    std::uint64_t p999 = 0;
  };

  [[nodiscard]] Snapshot snapshot() const noexcept;

  // Exposed for tests, which need to verify the precision bound rather than
  // trust it.
  [[nodiscard]] static std::size_t bucket_index(std::uint64_t value) noexcept;
  [[nodiscard]] static std::uint64_t bucket_upper_bound(std::size_t index) noexcept;

 private:
  std::array<std::uint64_t, kBucketCount> buckets_{};
  std::uint64_t count_ = 0;
  std::uint64_t sum_ = 0;
  std::uint64_t min_ = UINT64_MAX;
  std::uint64_t max_ = 0;
};

}  // namespace radio
