#include "radio/histogram.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>

using namespace radio;
using Catch::Approx;

namespace {

// Percentile answers are bucketed, so asserting exact equality against a
// hand-computed quantile is simply the wrong test -- the class documents a
// worst-case relative error of kRelativeError and that is the contract to
// check. Writing `== 10'000` here and then "fixing" the histogram to satisfy it
// would have destroyed the precision guarantee to make a bad test pass.
bool within_bound(std::uint64_t got, std::uint64_t want) {
  if (want == 0) return got == 0;
  const std::uint64_t diff = got > want ? got - want : want - got;
  return static_cast<double>(diff) / static_cast<double>(want) <=
         Histogram::kRelativeError;
}

}  // namespace

TEST_CASE("an empty histogram reports nothing rather than garbage") {
  Histogram h;
  CHECK(h.empty());
  CHECK(h.count() == 0);
  CHECK(h.min() == 0);
  CHECK(h.max() == 0);
  CHECK(h.mean() == 0.0);
  CHECK(h.percentile(50.0) == 0);
  CHECK(h.percentile(99.9) == 0);
}

TEST_CASE("small values are recorded exactly, not bucketed") {
  // The bottom octave is stored linearly, so sub-microsecond-scale latencies
  // carry no approximation at all.
  Histogram h;
  for (std::uint64_t v = 0; v < Histogram::kSubBuckets; ++v) {
    CHECK(Histogram::bucket_index(v) == v);
    CHECK(Histogram::bucket_upper_bound(Histogram::bucket_index(v)) == v);
  }
  for (std::uint64_t v = 0; v < 32; ++v) h.record(v);
  CHECK(h.count() == 32);
  CHECK(h.min() == 0);
  CHECK(h.max() == 31);
  CHECK(h.mean() == Approx(15.5));
}

TEST_CASE("buckets are contiguous and monotonic across the whole range") {
  // A gap or an overlap between adjacent buckets would silently lose or
  // double-count samples, so the mapping is verified rather than assumed.
  std::uint64_t previous_upper = 0;
  for (std::size_t i = 1; i < 700; ++i) {
    const std::uint64_t upper = Histogram::bucket_upper_bound(i);
    CHECK(upper > previous_upper);

    // The value one past the previous bucket's top must land in this bucket.
    CHECK(Histogram::bucket_index(previous_upper + 1) == i);
    // ...and this bucket's top must map back to this bucket.
    CHECK(Histogram::bucket_index(upper) == i);

    previous_upper = upper;
  }
}

TEST_CASE("the documented precision bound actually holds") {
  // The class promises a worst-case relative error of 1/kSubBuckets on any
  // percentile answer. That is a contract, so it gets tested rather than
  // trusted -- across seven orders of magnitude.
  for (std::uint64_t v = 1; v < 20'000'000ull; v += 1 + v / 7) {
    const std::size_t index = Histogram::bucket_index(v);
    const std::uint64_t upper = Histogram::bucket_upper_bound(index);

    REQUIRE(upper >= v);  // never understates
    const double error = static_cast<double>(upper - v) / static_cast<double>(v);
    REQUIRE(error <= Histogram::kRelativeError);
  }
  CHECK(Histogram::kRelativeError <= 0.032);  // ~3.1%
}

TEST_CASE("percentiles find the tail that a mean would hide") {
  // The reason this class exists. 990 packets at 10 ms and 10 packets at 500 ms
  // is a call with an audible problem, but the mean barely moves.
  Histogram h;
  for (int i = 0; i < 990; ++i) h.record(10'000);   // 10 ms
  for (int i = 0; i < 10; ++i) h.record(500'000);   // 500 ms

  CHECK(h.count() == 1000);
  CHECK(h.mean() == Approx(14'900.0));  // mean says "15 ms, looks fine"

  CHECK(within_bound(h.percentile(50.0), 10'000));
  CHECK(within_bound(h.percentile(95.0), 10'000));
  CHECK(within_bound(h.percentile(99.0), 10'000));

  // ...and the tail is right there in P99.9.
  CHECK(h.percentile(99.9) >= 400'000);
  CHECK(h.max() == 500'000);

  // A reviewer told "mean latency 14.9 ms" would conclude the call was good. A
  // reviewer told "P99.9 = 500 ms" would ask the right question.
}

TEST_CASE("percentiles of a uniform distribution land where expected") {
  Histogram h;
  for (std::uint64_t v = 1; v <= 10'000; ++v) h.record(v * 100);  // 100us..1s

  CHECK(h.count() == 10'000);
  CHECK(h.min() == 100);
  CHECK(h.max() == 1'000'000);

  // Within the documented 3.1% bound of the true quantiles.
  CHECK(within_bound(h.percentile(50.0), 500'000));
  CHECK(within_bound(h.percentile(95.0), 950'000));
  CHECK(within_bound(h.percentile(99.0), 990'000));
}

TEST_CASE("percentiles never exceed the largest value observed") {
  // A bucket's upper bound can sit well above anything actually recorded.
  // Reporting that bound as the P99 would overstate latency and invite the
  // wrong conclusion, so the answer is clamped to reality.
  Histogram h;
  h.record(1'000);
  h.record(1'001);
  h.record(1'002);

  CHECK(h.percentile(99.9) <= 1'002);
  CHECK(h.percentile(100.0) == 1'002);
  CHECK(h.percentile(0.0) == 1'000);
}

TEST_CASE("a single sample is reported exactly at every percentile") {
  Histogram h;
  h.record(42'000);
  CHECK(h.count() == 1);
  CHECK(h.min() == 42'000);
  CHECK(h.max() == 42'000);
  CHECK(h.percentile(50.0) == 42'000);
  CHECK(h.percentile(99.9) == 42'000);
  CHECK(h.mean() == Approx(42'000.0));
}

TEST_CASE("count, min, max and mean stay exact while percentiles approximate") {
  // Only percentiles are bucketed. The scalars are tracked directly, so they
  // carry no error at all -- worth knowing when you need one precise number.
  Histogram h;
  h.record(7);
  h.record(1'234'567);
  h.record(999);

  CHECK(h.count() == 3);
  CHECK(h.min() == 7);
  CHECK(h.max() == 1'234'567);
  CHECK(h.total() == 7 + 1'234'567 + 999);
  CHECK(h.mean() == Approx((7.0 + 1'234'567.0 + 999.0) / 3.0));
}

TEST_CASE("merging combines two histograms as if recorded together") {
  // Lets each thread or peer own an unsynchronised histogram and fold them
  // together at reporting time, with no lock on the recording path.
  Histogram a;
  Histogram b;
  Histogram combined;

  for (std::uint64_t v = 1; v <= 500; ++v) {
    a.record(v * 1'000);
    combined.record(v * 1'000);
  }
  for (std::uint64_t v = 501; v <= 1'000; ++v) {
    b.record(v * 1'000);
    combined.record(v * 1'000);
  }

  a.merge(b);
  CHECK(a.count() == combined.count());
  CHECK(a.min() == combined.min());
  CHECK(a.max() == combined.max());
  CHECK(a.total() == combined.total());
  CHECK(a.percentile(50.0) == combined.percentile(50.0));
  CHECK(a.percentile(99.0) == combined.percentile(99.0));
}

TEST_CASE("merging an empty histogram changes nothing") {
  Histogram a;
  a.record(500);
  a.record(600);
  const auto before = a.snapshot();

  a.merge(Histogram{});
  const auto after = a.snapshot();
  CHECK(after.count == before.count);
  CHECK(after.min == before.min);
  CHECK(after.max == before.max);
  CHECK(after.p99 == before.p99);
}

TEST_CASE("a snapshot bundles the numbers a report needs") {
  Histogram h;
  for (int i = 0; i < 1'000; ++i) h.record(50'000);
  h.record(900'000);

  const auto s = h.snapshot();
  CHECK(s.count == 1'001);
  CHECK(s.min == 50'000);
  CHECK(s.max == 900'000);
  CHECK(within_bound(s.p50, 50'000));
  CHECK(within_bound(s.p99, 50'000));
  CHECK(s.p999 >= 50'000);
}

TEST_CASE("reset returns the histogram to its initial state") {
  Histogram h;
  for (std::uint64_t v = 1; v <= 100; ++v) h.record(v * 1'000);
  REQUIRE(h.count() == 100);

  h.reset();
  CHECK(h.empty());
  CHECK(h.count() == 0);
  CHECK(h.min() == 0);
  CHECK(h.max() == 0);
  CHECK(h.percentile(99.0) == 0);
}

TEST_CASE("very large values do not run off the end of the bucket array") {
  // Input is uint64, so the mapping has to stay in range even for absurd
  // values. An out-of-bounds bucket index here would be a memory-safety bug in
  // a metrics path, which is a spectacularly bad place to have one.
  Histogram h;
  h.record(UINT64_MAX);
  h.record(UINT64_MAX / 2);
  h.record(1ull << 62);
  CHECK(h.count() == 3);
  CHECK(h.max() == UINT64_MAX);
  CHECK(Histogram::bucket_index(UINT64_MAX) < Histogram::kBucketCount);
}
