#include "radio/reorder_queue.hpp"

#include <algorithm>

#include "radio/serial.hpp"

namespace radio::audio {

namespace {

// The window is kCapacity wide in both directions: ahead of the cursor it is
// what we can store, behind it is how far back we are still willing to call an
// arrival "late" rather than "ancient". Using the same number for both is not a
// coincidence worth hiding — it is the one constant that says how much of the
// sequence space this queue reasons about at all.
constexpr std::int32_t kWindow =
    static_cast<std::int32_t>(ReorderQueue::kCapacity);

}  // namespace

const char* to_string(ReorderQueue::Insert v) noexcept {
  switch (v) {
    case ReorderQueue::Insert::Accepted:
      return "accepted";
    case ReorderQueue::Insert::Duplicate:
      return "duplicate";
    case ReorderQueue::Insert::Late:
      return "late";
    case ReorderQueue::Insert::TooOld:
      return "too_old";
    case ReorderQueue::Insert::TooFarAhead:
      return "too_far_ahead";
    case ReorderQueue::Insert::Oversized:
      return "oversized";
  }
  return "unknown";
}

const char* to_string(ReorderQueue::Step v) noexcept {
  switch (v) {
    case ReorderQueue::Step::Delivered:
      return "delivered";
    case ReorderQueue::Step::Gap:
      return "gap";
  }
  return "unknown";
}

void ReorderQueue::start(std::uint32_t first_sequence) noexcept {
  // Deliberately a no-op once running. A second start() would rewind the cursor
  // and silently replay a stretch of timeline; anything that legitimately needs
  // to move it calls resync(), which says so and is counted.
  if (started_) return;
  cursor_ = first_sequence;
  highest_ = first_sequence;
  started_ = true;
}

void ReorderQueue::resync(std::uint32_t sequence) noexcept {
  for (Slot& s : slots_) s.occupied = false;
  held_ = 0;
  cursor_ = sequence;
  highest_ = sequence;
  started_ = true;
  ++resyncs_;
}

void ReorderQueue::reset() noexcept {
  for (Slot& s : slots_) s.occupied = false;
  held_ = 0;
  cursor_ = 0;
  highest_ = 0;
  started_ = false;

  accepted_ = 0;
  duplicates_ = 0;
  late_ = 0;
  too_old_ = 0;
  too_far_ahead_ = 0;
  oversized_ = 0;
  expired_ = 0;
  discarded_ = 0;
  delivered_ = 0;
  gaps_ = 0;
  resyncs_ = 0;
}

ReorderQueue::Insert ReorderQueue::insert(const FrameInfo& info,
                                          ByteView payload,
                                          Micros now) noexcept {
  if (payload.size() > kMaxFramePayload) {
    ++oversized_;
    return Insert::Oversized;
  }

  const std::uint32_t sequence = info.header.sequence;

  // The first arrival defines where playout begins. If that packet was itself
  // reordered, the one it overtook arrives a moment later and is correctly
  // reported Late — which is the truth: by then we had already committed to
  // starting after it.
  if (!started_) start(sequence);

  const std::int32_t offset = serial::distance(cursor_, sequence);

  if (offset < 0) {
    if (offset > -kWindow) {
      ++late_;
      return Insert::Late;
    }
    ++too_old_;
    return Insert::TooOld;
  }

  if (offset >= kWindow) {
    ++too_far_ahead_;
    return Insert::TooFarAhead;
  }

  // Still ahead of the cursor, but its moment has passed anyway: the consumer
  // is about to walk over it. Equality is not late — a frame that arrives
  // exactly on its deadline is still playable.
  if (now > info.deadline_us) {
    ++late_;
    return Insert::Late;
  }

  Slot& slot = slots_[index(sequence)];

  // Occupancy implies the same sequence. Two sequences share a slot only if
  // they differ by a multiple of kCapacity, and the accepted range is exactly
  // kCapacity wide starting at the cursor, so no two of them fit in it at once.
  // Every slot the cursor passes is cleared, so nothing older can linger.
  if (slot.occupied) {
    ++duplicates_;
    return Insert::Duplicate;
  }

  slot.info = info;
  slot.size = static_cast<std::uint16_t>(payload.size());
  std::copy_n(payload.data(), payload.size(), slot.bytes.data());
  slot.occupied = true;

  if (held_ == 0 || serial::precedes(highest_, sequence)) highest_ = sequence;
  ++held_;
  ++accepted_;
  return Insert::Accepted;
}

bool ReorderQueue::peek(std::uint32_t sequence, Frame& out) const noexcept {
  if (!started_) return false;

  const std::int32_t offset = serial::distance(cursor_, sequence);
  if (offset < 0 || offset >= kWindow) return false;

  const Slot& slot = slots_[index(sequence)];
  // The sequence check is redundant given the window argument above, and is
  // kept because it is the guard that stops a future change to the window rule
  // from turning slot aliasing into silently wrong audio.
  if (!slot.occupied || slot.info.header.sequence != sequence) return false;

  out.info = slot.info;
  out.payload = ByteView(slot.bytes.data(), slot.size);
  return true;
}

bool ReorderQueue::peek_next(Frame& out) const noexcept {
  return peek(cursor_, out);
}

bool ReorderQueue::has_next() const noexcept {
  if (!started_) return false;
  const Slot& slot = slots_[index(cursor_)];
  return slot.occupied && slot.info.header.sequence == cursor_;
}

ReorderQueue::Step ReorderQueue::step_once() noexcept {
  Slot& slot = slots_[index(cursor_)];
  Step result = Step::Gap;
  if (slot.occupied && slot.info.header.sequence == cursor_) {
    slot.occupied = false;
    --held_;
    result = Step::Delivered;
  }
  ++cursor_;
  return result;
}

ReorderQueue::Step ReorderQueue::advance() noexcept {
  // Before the first arrival there is no timeline to advance along, but the
  // consumer still asked for a frame and still has to play something. Count the
  // gap and leave the cursor alone so the first packet can anchor it.
  if (!started_) {
    ++gaps_;
    return Step::Gap;
  }

  const Step result = step_once();
  if (result == Step::Delivered) {
    ++delivered_;
  } else {
    ++gaps_;
  }
  return result;
}

std::size_t ReorderQueue::skip_to(std::uint32_t sequence) noexcept {
  if (!started_) return 0;

  const std::int32_t offset = serial::distance(cursor_, sequence);
  if (offset <= 0) return 0;  // never rewinds

  const auto frames = static_cast<std::size_t>(offset);

  // Past one window there is nothing left to clear — the loop has already
  // visited every slot — so the rest of the jump is arithmetic. The bound
  // matters: recovering from a stall of several seconds is a legitimate jump of
  // hundreds of frames, and an unbounded loop there would be a stall of its
  // own.
  const std::size_t stepped = std::min(frames, kCapacity);
  for (std::size_t i = 0; i < stepped; ++i) {
    Slot& slot = slots_[index(cursor_)];
    if (slot.occupied && slot.info.header.sequence == cursor_) {
      slot.occupied = false;
      --held_;
      ++discarded_;
    }
    ++cursor_;
  }

  // Every position jumped is a playout tick the consumer got no audio for,
  // whether or not a frame happened to be sitting in it.
  gaps_ += frames;
  cursor_ = sequence;
  return frames;
}

std::size_t ReorderQueue::drop_expired(Micros now) noexcept {
  std::size_t dropped = 0;
  for (Slot& slot : slots_) {
    if (slot.occupied && now > slot.info.deadline_us) {
      slot.occupied = false;
      --held_;
      ++expired_;
      ++dropped;
    }
  }
  return dropped;
}

std::size_t ReorderQueue::depth() const noexcept {
  if (held_ == 0) return 0;
  // held_ != 0 guarantees some frame at or after the cursor, and highest_ is at
  // least that frame, so the distance cannot be negative.
  return static_cast<std::size_t>(serial::distance(cursor_, highest_)) + 1;
}

}  // namespace radio::audio
