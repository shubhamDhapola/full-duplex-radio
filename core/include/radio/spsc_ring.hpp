// Lock-free single-producer / single-consumer ring buffer.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace radio {

// Padding used to keep the producer's and the consumer's indices on separate
// cache lines. See the false-sharing note on SpscRing.
//
// 128 is the measured line size on Apple Silicon; x86-64 is 64. Using the
// larger value on both costs two cache lines of padding per ring — negligible
// against a buffer of thousands of samples — and being wrong in this direction
// only wastes space, whereas being wrong the other way costs throughput.
//
// std::hardware_destructive_interference_size would look more principled and is
// deliberately not used: GCC warns on it (-Winterference-size) because its
// value depends on -mtune and is therefore not ABI-stable, and libc++ reports
// 256 here rather than the hardware's 128. An explicit constant with a stated
// reason is more honest than a portable-looking one whose value nobody can
// predict.
inline constexpr std::size_t kCacheLineBytes = 128;

// A bounded queue that one thread writes and one thread reads, with no locks.
//
// WHY THIS EXISTS
//
// Audio capture and playback run on a high-priority thread supplied by the
// operating system, with a hard deadline: the hardware consumes a buffer every
// 20 ms whether or not it has been filled. Encoding, decoding and networking
// run on ordinary threads. Data must cross between them.
//
// A mutex cannot be used for that hand-off. If the encoder thread holds the
// lock when the audio callback fires, the callback waits — and a waiting audio
// callback is an audible click. It does not matter that the wait is usually
// short; "usually" is not a property real-time code can rely on.
//
// SPSC IS THE SIMPLIFICATION THAT MAKES THIS TRACTABLE
//
// Exactly one thread pushes and exactly one thread pops. That constraint is
// what keeps this correct in about forty lines: each index has a single writer,
// so there is never contention over a value, only publication of it. Lock-free
// queues become genuinely difficult with multiple producers, and this project
// never needs one.
//
// The constraint is not enforceable by the type system, so it is the caller's
// obligation: push() may only ever be called from one thread and pop() from one
// other. Two threads calling push() concurrently is undefined behaviour, not a
// contended slow path.
//
// ON OVERFLOW: THE NEWEST IS REFUSED, NOT THE OLDEST DISCARDED
//
// Real-time systems usually want to discard the *oldest* data when a queue
// fills, because stale audio is worth less than fresh audio. That policy cannot
// live here: dropping the oldest means advancing the read index, which the
// consumer owns, so the producer touching it would make this a two-writer
// structure and forfeit the correctness argument above.
//
// So push() refuses and counts the refusal, and the drop-oldest policy lives
// one layer up — in the jitter buffer, which owns both ends of its own data and
// can discard by playout deadline, which is the meaningful criterion anyway.
//
// This is not a workaround. A full ring here means the consumer is further
// behind than the buffer is deep, which for a correctly sized buffer means a
// starved thread or a bug rather than a condition to handle gracefully. Sizing
// it so that cannot happen and counting it when it does is more useful than
// silently papering over it: overflows() being non-zero is a finding.
template <class T, std::size_t Capacity>
class SpscRing {
 public:
  static_assert(Capacity >= 2, "a ring needs at least two slots");
  static_assert((Capacity & (Capacity - 1)) == 0,
                "Capacity must be a power of two so the index wrap is a mask "
                "rather than a division");
  static_assert(
      std::is_trivially_copyable_v<T>,
      "bulk transfer copies raw bytes; a type needing construction or "
      "destruction would be silently mishandled");

  using value_type = T;

  [[nodiscard]] static constexpr std::size_t capacity() noexcept {
    return Capacity;
  }

  // ---------------------------------------------------------------- producer

  // Returns false if the ring is full, having counted the refusal.
  [[nodiscard]] bool push(const T& value) noexcept {
    // Relaxed is correct for our own index: this thread is its only writer, so
    // there is nothing to synchronise with. Using acquire here would be a
    // pointless barrier on the hot path.
    const std::size_t write = write_.index.load(std::memory_order_relaxed);

    // Acquire on the other side's index, because we need to see the consumer's
    // reads as having completed before we reuse the slots it freed.
    const std::size_t read = read_.index.load(std::memory_order_acquire);

    if (write - read == Capacity) {
      ++overflows_;
      return false;
    }

    slots_[write & kMask] = value;

    // Release publishes the slot write above. Without it, the consumer could
    // observe the new index while still seeing stale slot contents — the
    // classic reordering bug, and one that x86 hides almost perfectly while
    // arm64 does not.
    write_.index.store(write + 1, std::memory_order_release);
    return true;
  }

  // Copies up to `count` items in and returns how many were accepted.
  //
  // Bulk transfer is the operation that actually matters. A 20 ms frame is 960
  // samples; pushing those one at a time would be 960 acquire loads and 960
  // release stores for one frame, which is absurd when the whole point is to be
  // cheap. In bulk it is one acquire, at most two memcpys, and one release.
  [[nodiscard]] std::size_t push_bulk(const T* source,
                                      std::size_t count) noexcept {
    if (count == 0) return 0;

    const std::size_t write = write_.index.load(std::memory_order_relaxed);
    const std::size_t read = read_.index.load(std::memory_order_acquire);
    const std::size_t free_slots = Capacity - (write - read);

    if (free_slots == 0) {
      ++overflows_;
      return 0;
    }

    const std::size_t accepted = std::min(count, free_slots);
    const std::size_t offset = write & kMask;

    // Two copies when the run wraps past the end of the storage, one otherwise.
    const std::size_t first = std::min(accepted, Capacity - offset);
    std::copy_n(source, first, slots_.data() + offset);
    if (accepted > first) {
      std::copy_n(source + first, accepted - first, slots_.data());
    }

    if (accepted < count) ++overflows_;
    write_.index.store(write + accepted, std::memory_order_release);
    return accepted;
  }

  // ---------------------------------------------------------------- consumer

  [[nodiscard]] bool pop(T& out) noexcept {
    const std::size_t read = read_.index.load(std::memory_order_relaxed);
    const std::size_t write = write_.index.load(std::memory_order_acquire);

    if (read == write) return false;

    out = slots_[read & kMask];
    read_.index.store(read + 1, std::memory_order_release);
    return true;
  }

  [[nodiscard]] std::size_t pop_bulk(T* destination,
                                     std::size_t count) noexcept {
    if (count == 0) return 0;

    const std::size_t read = read_.index.load(std::memory_order_relaxed);
    const std::size_t write = write_.index.load(std::memory_order_acquire);
    const std::size_t available = write - read;

    if (available == 0) return 0;

    const std::size_t taken = std::min(count, available);
    const std::size_t offset = read & kMask;

    const std::size_t first = std::min(taken, Capacity - offset);
    std::copy_n(slots_.data() + offset, first, destination);
    if (taken > first) {
      std::copy_n(slots_.data(), taken - first, destination + first);
    }

    read_.index.store(read + taken, std::memory_order_release);
    return taken;
  }

  // Discards up to `count` items without copying them out. Used by the consumer
  // to catch up deliberately after falling behind.
  [[nodiscard]] std::size_t discard(std::size_t count) noexcept {
    const std::size_t read = read_.index.load(std::memory_order_relaxed);
    const std::size_t write = write_.index.load(std::memory_order_acquire);
    const std::size_t dropped = std::min(count, write - read);
    if (dropped != 0) {
      read_.index.store(read + dropped, std::memory_order_release);
    }
    return dropped;
  }

  // ------------------------------------------------------------ observation

  // Approximate while both threads are running: by the time it returns, the
  // other side may have moved. Exact only when the caller knows one side is
  // idle, which is the case in tests and at shutdown.
  [[nodiscard]] std::size_t size() const noexcept {
    const std::size_t write = write_.index.load(std::memory_order_acquire);
    const std::size_t read = read_.index.load(std::memory_order_acquire);
    return write - read;
  }

  [[nodiscard]] bool empty() const noexcept { return size() == 0; }
  [[nodiscard]] bool full() const noexcept { return size() == Capacity; }

  // Pushes refused because the ring was full, plus bulk pushes that were only
  // partly accepted. Producer-side only, so it needs no synchronisation.
  [[nodiscard]] std::uint64_t overflows() const noexcept { return overflows_; }

  // Only safe when neither thread is running.
  void reset() noexcept {
    write_.index.store(0, std::memory_order_relaxed);
    read_.index.store(0, std::memory_order_relaxed);
    overflows_ = 0;
  }

 private:
  static constexpr std::size_t kMask = Capacity - 1;

  // The indices are deliberately monotonic rather than pre-wrapped, and are
  // masked only when used to address a slot.
  //
  // With wrapped indices, `write == read` would mean both empty and full, and
  // distinguishing them costs either a sacrificed slot or a separate flag that
  // has to be updated atomically alongside the index. Monotonic indices make
  // `write - read` the exact occupancy and the two states unambiguous.
  //
  // They rely on unsigned wraparound at 2^64, which is defined behaviour. At
  // one 20 ms frame per push that is roughly 11 billion years, and the
  // subtraction stays correct across the wrap regardless.
  //
  // FALSE SHARING is why each index sits in its own padded struct. Without the
  // padding both would share a cache line, so the producer's store to `write`
  // would invalidate that line in the consumer's cache and vice versa — the two
  // threads would fight over one line on every single operation despite never
  // touching the same variable. It is a several-fold throughput difference and
  // it is invisible in the source unless the padding is there to point at it.
  struct alignas(kCacheLineBytes) PaddedIndex {
    std::atomic<std::size_t> index{0};
  };

  static_assert(sizeof(PaddedIndex) >= kCacheLineBytes,
                "the padding is the point; without it the two indices share a "
                "cache line and the threads contend on every operation");

  PaddedIndex write_;
  PaddedIndex read_;

  // Producer-only, so a plain integer rather than an atomic.
  std::uint64_t overflows_ = 0;

  // Inline storage. The ring is allocated once by whoever owns it, and never
  // allocates again — there is no heap access on any path here.
  std::array<T, Capacity> slots_{};
};

// The two shapes this project uses.

// Captured or decoded audio, 16-bit mono at 48 kHz. 8192 samples is about
// 170 ms, which is far deeper than any correct configuration needs — the point
// is that overflow indicates a starved thread rather than ordinary
// backpressure.
using PcmRing = SpscRing<std::int16_t, 8192>;

static_assert(PcmRing::capacity() * sizeof(std::int16_t) == 16384,
              "PcmRing should be 16 KB of samples");

}  // namespace radio
