#include "radio/spsc_ring.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <numeric>
#include <thread>
#include <vector>

using namespace radio;

namespace {
using Ring8 = SpscRing<int, 8>;
}

TEST_CASE("a fresh ring is empty") {
  Ring8 ring;
  CHECK(ring.empty());
  CHECK_FALSE(ring.full());
  CHECK(ring.size() == 0);
  CHECK(ring.capacity() == 8);
  CHECK(ring.overflows() == 0);

  int value = 42;
  CHECK_FALSE(ring.pop(value));
  CHECK(value == 42);  // untouched on failure
}

TEST_CASE("items come out in the order they went in") {
  Ring8 ring;
  for (int i = 0; i < 5; ++i) REQUIRE(ring.push(i));
  CHECK(ring.size() == 5);

  for (int expected = 0; expected < 5; ++expected) {
    int value = -1;
    REQUIRE(ring.pop(value));
    CHECK(value == expected);
  }
  CHECK(ring.empty());
}

TEST_CASE("the ring holds exactly its capacity, no slot sacrificed") {
  // Monotonic indices rather than pre-wrapped ones are what make this possible.
  // With wrapped indices, write == read would mean both empty and full, and the
  // usual fix is to leave one slot permanently unused.
  Ring8 ring;
  for (int i = 0; i < 8; ++i) REQUIRE(ring.push(i));
  CHECK(ring.full());
  CHECK(ring.size() == 8);
}

TEST_CASE("a full ring refuses the newest and counts the refusal") {
  Ring8 ring;
  for (int i = 0; i < 8; ++i) REQUIRE(ring.push(i));

  CHECK_FALSE(ring.push(99));
  CHECK_FALSE(ring.push(99));
  CHECK(ring.overflows() == 2);

  // The refused items are gone; the ones already queued are intact. Refusing
  // the newest keeps this a single-writer structure -- discarding the oldest
  // would mean the producer advancing the consumer's index.
  int value = -1;
  REQUIRE(ring.pop(value));
  CHECK(value == 0);
}

TEST_CASE("indices wrap without corrupting anything") {
  // Drive far past capacity so the mask wraps many times. An off-by-one in the
  // masking shows up here and nowhere else.
  Ring8 ring;
  for (int round = 0; round < 1000; ++round) {
    REQUIRE(ring.push(round));
    int value = -1;
    REQUIRE(ring.pop(value));
    REQUIRE(value == round);
  }
  CHECK(ring.empty());
  CHECK(ring.overflows() == 0);
}

TEST_CASE("a partly drained ring keeps working across the wrap") {
  Ring8 ring;
  int next_in = 0;
  int next_out = 0;

  for (int round = 0; round < 500; ++round) {
    // Three in, two out: the occupancy drifts up until it saturates, and the
    // window slides across the storage boundary repeatedly.
    for (int i = 0; i < 3; ++i) {
      if (ring.push(next_in)) ++next_in;
    }
    for (int i = 0; i < 2; ++i) {
      int value = -1;
      if (ring.pop(value)) {
        REQUIRE(value == next_out);
        ++next_out;
      }
    }
  }
  CHECK(next_out > 900);  // real throughput happened
}

TEST_CASE("bulk transfer moves whole frames at once") {
  // The operation that matters in practice: a 20 ms frame is 960 samples, and
  // pushing those individually would be 960 acquire loads for one frame.
  SpscRing<std::int16_t, 2048> ring;
  std::vector<std::int16_t> frame(960);
  std::iota(frame.begin(), frame.end(), std::int16_t{0});

  CHECK(ring.push_bulk(frame.data(), frame.size()) == 960);
  CHECK(ring.size() == 960);

  std::vector<std::int16_t> out(960, -1);
  CHECK(ring.pop_bulk(out.data(), out.size()) == 960);
  CHECK(out == frame);
  CHECK(ring.empty());
}

TEST_CASE("bulk transfer handles the wrap, which is a two-copy path") {
  // Position the window so a bulk run straddles the end of the storage. This is
  // the branch a single-memcpy implementation gets wrong.
  SpscRing<int, 16> ring;
  for (int i = 0; i < 12; ++i) REQUIRE(ring.push(i));
  for (int i = 0; i < 12; ++i) {
    int value = -1;
    REQUIRE(ring.pop(value));
  }
  REQUIRE(ring.empty());  // read and write both at 12, four slots to the end

  std::vector<int> source(10);
  std::iota(source.begin(), source.end(), 100);
  CHECK(ring.push_bulk(source.data(), source.size()) == 10);

  std::vector<int> out(10, -1);
  CHECK(ring.pop_bulk(out.data(), out.size()) == 10);
  CHECK(out == source);
}

TEST_CASE("bulk push accepts what it can and reports the shortfall") {
  Ring8 ring;
  std::vector<int> source(20);
  std::iota(source.begin(), source.end(), 0);

  CHECK(ring.push_bulk(source.data(), source.size()) == 8);
  CHECK(ring.full());
  CHECK(ring.overflows() == 1);  // a partial acceptance is still an overflow

  CHECK(ring.push_bulk(source.data(), 1) == 0);
  CHECK(ring.overflows() == 2);

  // The first eight made it, in order.
  for (int expected = 0; expected < 8; ++expected) {
    int value = -1;
    REQUIRE(ring.pop(value));
    CHECK(value == expected);
  }
}

TEST_CASE("bulk pop takes what is there and no more") {
  Ring8 ring;
  for (int i = 0; i < 3; ++i) REQUIRE(ring.push(i));

  std::vector<int> out(10, -1);
  CHECK(ring.pop_bulk(out.data(), out.size()) == 3);
  CHECK(out[0] == 0);
  CHECK(out[2] == 2);
  CHECK(out[3] == -1);  // beyond what was available, left alone
  CHECK(ring.empty());

  CHECK(ring.pop_bulk(out.data(), out.size()) == 0);
}

TEST_CASE("zero-length bulk operations are no-ops") {
  Ring8 ring;
  REQUIRE(ring.push(1));
  CHECK(ring.push_bulk(nullptr, 0) == 0);
  CHECK(ring.pop_bulk(nullptr, 0) == 0);
  CHECK(ring.size() == 1);
  CHECK(ring.overflows() == 0);
}

TEST_CASE("discard lets the consumer catch up without copying") {
  Ring8 ring;
  for (int i = 0; i < 6; ++i) REQUIRE(ring.push(i));

  CHECK(ring.discard(4) == 4);
  CHECK(ring.size() == 2);

  int value = -1;
  REQUIRE(ring.pop(value));
  CHECK(value == 4);  // the first four were skipped

  CHECK(ring.discard(100) == 1);  // clamped to what is present
  CHECK(ring.empty());
  CHECK(ring.discard(1) == 0);
}

TEST_CASE("reset returns the ring to its initial state") {
  Ring8 ring;
  for (int i = 0; i < 8; ++i) REQUIRE(ring.push(i));
  REQUIRE_FALSE(ring.push(99));
  REQUIRE(ring.overflows() == 1);

  ring.reset();
  CHECK(ring.empty());
  CHECK(ring.overflows() == 0);
  REQUIRE(ring.push(7));
  int value = -1;
  REQUIRE(ring.pop(value));
  CHECK(value == 7);
}

TEST_CASE("a single producer and consumer never lose, duplicate or reorder") {
  // The real proof, and the reason this file exists.
  //
  // The producer retries on refusal, so nothing is legitimately dropped. The
  // consumer therefore must observe exactly 0, 1, 2, ... N-1 in that order. Any
  // lost item, any duplicate, and any reordering is a detectable failure rather
  // than something that has to be inferred from a crash.
  //
  // Run this under -fsanitize=thread to also prove there is no data race:
  //   cmake -B build-tsan -DRADIO_SANITIZE_THREAD=ON -DCMAKE_BUILD_TYPE=Debug
  constexpr int kItems = 500'000;
  SpscRing<int, 1024> ring;
  std::atomic<bool> producer_done{false};

  std::thread producer([&] {
    for (int i = 0; i < kItems; ++i) {
      while (!ring.push(i)) {
        // Spin rather than sleep: this is a test, and yielding here would hide
        // the interleavings most likely to expose a missing barrier.
        std::this_thread::yield();
      }
    }
    producer_done.store(true, std::memory_order_release);
  });

  int expected = 0;
  int received = 0;
  bool ordering_violation = false;

  while (expected < kItems) {
    int value = -1;
    if (ring.pop(value)) {
      if (value != expected) ordering_violation = true;
      ++expected;
      ++received;
    } else if (producer_done.load(std::memory_order_acquire) && ring.empty()) {
      break;
    }
  }

  producer.join();

  CHECK_FALSE(ordering_violation);
  CHECK(received == kItems);
  CHECK(ring.empty());
}

TEST_CASE("bulk transfer is also race-free and order-preserving") {
  // Same invariant, exercised through the bulk path, where the two-copy wrap
  // branch and the index publication interact.
  constexpr int kItems = 400'000;
  SpscRing<int, 1024> ring;
  std::atomic<bool> producer_done{false};

  std::thread producer([&] {
    std::vector<int> batch(97);  // deliberately not a divisor of the capacity
    int next = 0;
    while (next < kItems) {
      const std::size_t want = std::min<std::size_t>(
          batch.size(), static_cast<std::size_t>(kItems - next));
      for (std::size_t i = 0; i < want; ++i) {
        batch[i] = next + static_cast<int>(i);
      }
      std::size_t offset = 0;
      while (offset < want) {
        const std::size_t accepted =
            ring.push_bulk(batch.data() + offset, want - offset);
        if (accepted == 0) std::this_thread::yield();
        offset += accepted;
      }
      next += static_cast<int>(want);
    }
    producer_done.store(true, std::memory_order_release);
  });

  std::vector<int> out(131);  // also not a divisor
  int expected = 0;
  bool violation = false;

  while (expected < kItems) {
    const std::size_t taken = ring.pop_bulk(out.data(), out.size());
    for (std::size_t i = 0; i < taken; ++i) {
      if (out[i] != expected) violation = true;
      ++expected;
    }
    if (taken == 0 && producer_done.load(std::memory_order_acquire) &&
        ring.empty()) {
      break;
    }
  }

  producer.join();
  CHECK_FALSE(violation);
  CHECK(expected == kItems);
}

TEST_CASE("the ring allocates nothing and stores inline") {
  // Storage is a member array, so constructing one performs no heap access.
  // Asserted structurally rather than by instrumenting the allocator: the size
  // could only be this if the slots were inline.
  static_assert(sizeof(SpscRing<std::int16_t, 1024>) >=
                1024 * sizeof(std::int16_t));
  static_assert(sizeof(PcmRing) >= PcmRing::capacity() * sizeof(std::int16_t));
  // ...plus two padded indices, so the object is at least that much larger.
  static_assert(sizeof(SpscRing<std::int16_t, 1024>) >=
                1024 * sizeof(std::int16_t) + 2 * kCacheLineBytes);
  SUCCEED();
}
