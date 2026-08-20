#include "radio/seq_tracker.hpp"

#include "radio/serial.hpp"

namespace radio {
namespace {
constexpr std::size_t kBitsPerWord = 64;
}  // namespace

bool SeqTracker::test(std::uint32_t seq) const noexcept {
  const std::size_t s = slot(seq);
  return ((words_[s / kBitsPerWord] >> (s % kBitsPerWord)) & 1ull) != 0ull;
}

void SeqTracker::set(std::uint32_t seq) noexcept {
  const std::size_t s = slot(seq);
  words_[s / kBitsPerWord] |= (1ull << (s % kBitsPerWord));
}

void SeqTracker::clear(std::uint32_t seq) noexcept {
  const std::size_t s = slot(seq);
  words_[s / kBitsPerWord] &= ~(1ull << (s % kBitsPerWord));
}

void SeqTracker::clear_range_exclusive(std::uint32_t from,
                                       std::uint32_t to) noexcept {
  // The single most important few lines in this class. When the high-water mark
  // jumps forward, the slots it moves across correspond to sequence numbers we
  // have NOT received — but because the window is circular, those same slots
  // may still hold bits set a full lap (1024 packets) ago. Leaving them set
  // makes a later reordered arrival look like a duplicate.
  //
  // The failure mode is what makes this worth care: it cannot happen until the
  // stream has run for a full window, so at 50 packets/second every test
  // shorter than ~20 seconds passes regardless.
  const std::int64_t span = serial::distance(from, to);
  if (span <= 0) return;
  if (span >= static_cast<std::int64_t>(kWindowSize)) {
    // Jumped clear past the whole window: nothing retained can be trusted.
    words_.fill(0);
    return;
  }
  for (std::uint32_t s = from + 1u; s != to; ++s) clear(s);
}

SeqTracker::Verdict SeqTracker::observe(std::uint32_t seq) noexcept {
  if (!started_) {
    started_ = true;
    base_ = seq;
    highest_ = seq;
    words_.fill(0);
    set(seq);
    received_ = 1;
    return Verdict::First;
  }

  const std::int32_t ahead = serial::distance(highest_, seq);

  if (ahead > 0) {
    clear_range_exclusive(highest_, seq);
    highest_ = seq;
    set(seq);
    ++received_;
    return Verdict::InOrder;
  }

  if (ahead == 0) {
    ++duplicates_;
    return Verdict::Duplicate;
  }

  // Behind the high-water mark: either a reordered arrival we can still place,
  // or something so old the window no longer remembers whether we saw it.
  // Those must stay distinct, because "I know this is a duplicate" and "I have
  // no idea" are different facts and only one of them is evidence about the
  // network.
  const std::int64_t age = -static_cast<std::int64_t>(ahead);
  if (age >= static_cast<std::int64_t>(kWindowSize)) {
    ++too_old_;
    return Verdict::TooOld;
  }

  if (test(seq)) {
    ++duplicates_;
    return Verdict::Duplicate;
  }

  set(seq);
  ++received_;
  ++reordered_;
  // A packet may arrive before the first one we ever saw. Pulling the baseline
  // back keeps expected() equal to the span we actually observed, so received_
  // can never exceed it and lost() can never go negative.
  if (serial::precedes(seq, base_)) base_ = seq;
  return Verdict::Reordered;
}

void SeqTracker::reset() noexcept { *this = SeqTracker{}; }

std::uint64_t SeqTracker::expected() const noexcept {
  if (!started_) return 0;
  const std::int32_t span = serial::distance(base_, highest_);
  if (span < 0) return 0;  // unreachable: base_ is pulled back on reorder
  return static_cast<std::uint64_t>(span) + 1ull;
}

std::uint64_t SeqTracker::lost() const noexcept {
  const std::uint64_t exp = expected();
  // Clamped rather than allowed to wrap. This is the bug the whole class exists
  // to prevent: on unsigned types, "1 - 2" is 18 quintillion, and a loss
  // counter that can report 1.8e19% is worse than no loss counter at all.
  return exp > received_ ? exp - received_ : 0ull;
}

double SeqTracker::loss_fraction() const noexcept {
  const std::uint64_t exp = expected();
  if (exp == 0) return 0.0;
  return static_cast<double>(lost()) / static_cast<double>(exp);
}

const char* to_string(SeqTracker::Verdict v) noexcept {
  switch (v) {
    case SeqTracker::Verdict::First:
      return "first";
    case SeqTracker::Verdict::InOrder:
      return "in_order";
    case SeqTracker::Verdict::Reordered:
      return "reordered";
    case SeqTracker::Verdict::Duplicate:
      return "duplicate";
    case SeqTracker::Verdict::TooOld:
      return "too_old";
  }
  return "invalid";
}

}  // namespace radio
