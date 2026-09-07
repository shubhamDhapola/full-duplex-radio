// Per-stream sequence accounting: loss, duplication, reordering.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace radio {

// Tracks what has been received on one media stream and classifies each
// arrival.
//
// The data structure is a sliding-window bitmap over the sequence space: one
// bit per recent sequence number, in a circular buffer anchored at the highest
// sequence seen. That one structure answers all three questions this project
// needs to keep separate:
//
//   loss        expected span minus unique arrivals
//   duplicates  the bit was already set
//   reordering  the bit was behind the high-water mark
//
// The tempting alternative is RFC 3550's approach of counting arrivals and
// subtracting from the expected span. That gives loss for free but cannot
// distinguish a duplicate from a genuine arrival, so a network that duplicates
// packets reports *negative* loss — which is exactly the kind of nonsense
// number that makes a benchmark table untrustworthy.
//
// A window of 1024 packets is about 20 seconds of a 50 packet/second voice
// stream, which is two orders of magnitude beyond any jitter buffer this
// project will run. Anything older than that is genuinely ancient rather than
// merely late, so collapsing it into a single `TooOld` class loses nothing. The
// whole structure is 128 bytes, so there is no reason to be stingier.
//
// This is the same shape as an IPsec/DTLS anti-replay window, and when
// encryption arrives (M8) replay protection is this class with the verdict
// interpreted as accept/reject rather than as telemetry.
class SeqTracker {
 public:
  static constexpr std::size_t kWindowSize = 1024;

  enum class Verdict : std::uint8_t {
    First,      // first packet on the stream; established the baseline
    InOrder,    // advanced the high-water mark
    Reordered,  // filled a gap behind the high-water mark
    Duplicate,  // already seen
    TooOld,     // older than the window, so duplicate-vs-new is unknowable
  };

  // Deliberately *not* [[nodiscard]]. The attribute belongs on functions whose
  // return value is the entire point — discarding parse_media()'s result means
  // you skipped validation, which is always a bug. Here the return is
  // supplementary: the call's main job is to record the arrival, and plenty of
  // callers legitimately want the accounting without caring how this particular
  // packet was classified. Marking it nodiscard would train people to write
  // `(void)t.observe(s)`, and a codebase where the attribute is routinely
  // silenced is one where it no longer carries a signal.
  Verdict observe(std::uint32_t seq) noexcept;

  void reset() noexcept;

  [[nodiscard]] bool started() const noexcept { return started_; }
  [[nodiscard]] std::uint32_t base() const noexcept { return base_; }
  [[nodiscard]] std::uint32_t highest() const noexcept { return highest_; }

  // Unique arrivals. Duplicates are excluded, which is what makes lost()
  // meaningful.
  [[nodiscard]] std::uint64_t received() const noexcept { return received_; }
  [[nodiscard]] std::uint64_t duplicates() const noexcept {
    return duplicates_;
  }
  [[nodiscard]] std::uint64_t reordered() const noexcept { return reordered_; }
  [[nodiscard]] std::uint64_t too_old() const noexcept { return too_old_; }

  // Packets the sender must have sent, from the sequence span actually
  // observed.
  [[nodiscard]] std::uint64_t expected() const noexcept;

  // Never negative by construction: every unique arrival lies within the span.
  [[nodiscard]] std::uint64_t lost() const noexcept;

  [[nodiscard]] double loss_fraction() const noexcept;

 private:
  static constexpr std::size_t kWords = kWindowSize / 64;

  [[nodiscard]] static std::size_t slot(std::uint32_t seq) noexcept {
    return seq % kWindowSize;
  }
  [[nodiscard]] bool test(std::uint32_t seq) const noexcept;
  void set(std::uint32_t seq) noexcept;
  void clear(std::uint32_t seq) noexcept;
  // Clears the slots for (from, to] so a newly advanced range cannot inherit
  // stale bits from the previous lap around the circular window.
  void clear_range_exclusive(std::uint32_t from, std::uint32_t to) noexcept;

  std::array<std::uint64_t, kWords> words_{};
  std::uint32_t base_ = 0;
  std::uint32_t highest_ = 0;
  std::uint64_t received_ = 0;
  std::uint64_t duplicates_ = 0;
  std::uint64_t reordered_ = 0;
  std::uint64_t too_old_ = 0;
  bool started_ = false;
};

[[nodiscard]] const char* to_string(SeqTracker::Verdict v) noexcept;

}  // namespace radio
