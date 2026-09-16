// Sequence-ordered holding pen for received media frames, ahead of playout.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "radio/bytes.hpp"
#include "radio/clock.hpp"
#include "radio/proto.hpp"

namespace radio::audio {

// Holds arrived-but-not-yet-played frames in sequence order, so the decoder can
// be fed one frame per playout tick regardless of the order the network chose.
//
// WHY THIS IS A SEPARATE THING FROM THE JITTER BUFFER
//
// The jitter buffer is a *policy*: how deep to buffer, when to re-anchor the
// timeline, whether a missing frame should be concealed or recovered from the
// next packet's redundancy. This class is the *mechanism* underneath it —
// storage, ordering, and the classification of every arrival. Splitting them
// means the adaptive controller in M4 can change every one of those policies
// without touching a data structure whose correctness is the fiddly part.
//
// The split shows up in the interface: nothing here decides anything. insert()
// reports what an arrival was, advance() steps the cursor, and the caller
// chooses what to do about it.
//
// THE PLAY CURSOR IS THE WHOLE IDEA
//
// One sequence number, `cursor_`, is the frame the consumer wants next. It only
// moves forward, one frame at a time, driven by the consumer's clock. Every
// other question this class answers is a comparison against it:
//
//   ahead of the cursor, inside the window   store it, we will want it soon
//   ahead of the cursor, beyond the window   TooFarAhead: the stream restarted
//   behind the cursor                        Late: we already played over it
//   far behind the cursor                    TooOld: not plausibly this session
//
// Ordering is therefore not something this class does; it is something the
// cursor makes unnecessary. There is no sort, no comparator and no linked list,
// because frames are stored at `sequence % kCapacity` and "the next one" is
// always one array index. Insert and lookup are both a masked array access.
//
// ON STORING BY sequence % kCapacity
//
// The same trick SeqTracker uses, and it carries the same obligation: slots are
// reused every lap around the sequence space, so a slot's contents must be
// checked against the sequence that was asked for, never trusted because the
// slot is occupied. The window is kept exactly kCapacity wide starting at the
// cursor, which makes that check a tautology — two sequences collide only if
// they differ by a multiple of kCapacity, and no two such values fit in one
// window — but it is asserted rather than assumed, because the day the window
// rule changes is the day the aliasing becomes real and silent.
//
// THREADING: NONE. This is an ordinary single-threaded object.
//
// In a pull-model pipeline the consumer thread owns it, along with the decoder.
// The network thread does not insert into it directly; it hands whole datagrams
// across an SpscRing, and the consumer drains that ring into this queue before
// pulling. That ordering is deliberate — it keeps the lock-free reasoning
// confined to SpscRing, which is forty lines and heavily tested, instead of
// spreading it over a structure with a cursor, a window and eight counters.
class ReorderQueue {
 public:
  // 64 frames is 1.28 s at 20 ms. That is not the buffer depth — depth is a few
  // frames and is the jitter buffer's decision. It is the range of sequence
  // numbers the queue can *represent*, and it wants to be far wider than any
  // reorder distance a LAN produces (typically 0, occasionally 1-2 frames) so
  // that TooFarAhead means "the sender restarted" rather than "the network was
  // briefly untidy".
  static constexpr std::size_t kCapacity = 64;

  static_assert((kCapacity & (kCapacity - 1)) == 0,
                "kCapacity must be a power of two so the slot index is a mask");

  // Per-slot payload storage. A 20 ms Opus frame at 32 kbps is about 80 bytes
  // and VBR peaks well under 200; 512 leaves room for 64 kbps or 40 ms frames
  // without the whole queue exceeding 35 KB. A payload larger than this is
  // refused and counted rather than truncated, because a truncated Opus packet
  // decodes to plausible-sounding garbage and would be diagnosed as a codec
  // bug.
  static constexpr std::size_t kMaxFramePayload = 512;

  struct FrameInfo {
    // The wire header as parsed, so flags() and talkspurt_start() travel with
    // the frame instead of being re-derived at playout.
    proto::MediaHeader header;

    // When the socket saw it, for tracing and for the jitter estimator.
    Micros arrival_us = 0;

    // When this frame must be handed to the decoder.
    //
    // Computed by the caller, not here, because it comes from the mapping
    // between the sender's `timestamp` and our playout clock — and that mapping
    // re-anchors at talkspurt boundaries, which is policy. See the note on
    // insert() for the one thing the queue does with it.
    Micros deadline_us = 0;
  };

  struct Frame {
    FrameInfo info;

    // A view into the queue's own storage, not a copy. Valid until the slot is
    // reused, which cannot happen before the cursor passes this frame and a
    // sequence kCapacity further on arrives. In practice: use it, then
    // advance(). This mirrors proto::MediaPacket, which also views the buffer
    // it was parsed from rather than copying a payload on the hot path.
    ByteView payload;
  };

  // What an arrival was. Every one of these is counted separately because they
  // have different fixes, and spec §6.2 requires late and lost never be
  // conflated.
  enum class Insert : std::uint8_t {
    Accepted,
    Duplicate,    // already held; the network or a relay copied it
    Late,         // its playout moment has gone: the buffer is too shallow
    TooOld,       // so far behind that late-versus-stray is unknowable
    TooFarAhead,  // beyond the window: sender restarted, or a long stall
    Oversized,    // payload larger than a slot
  };

  // What one step of the cursor produced.
  enum class Step : std::uint8_t {
    Delivered,  // a frame was there
    Gap,        // nothing was there; the caller must conceal or recover
  };

  // Anchors the cursor. Optional — the first insert() anchors on its own
  // sequence if this was not called, which is the right behaviour when a stream
  // simply starts: whatever arrived first is where playout begins, even if that
  // packet was itself reordered and an earlier one shows up a moment later as
  // Late.
  void start(std::uint32_t first_sequence) noexcept;

  // Drops everything held, moves the cursor to `sequence`, and counts a resync.
  // The caller's response to TooFarAhead, and to a stream_id change.
  void resync(std::uint32_t sequence) noexcept;

  // Full reset, counters included. Not for use mid-stream; that is resync().
  void reset() noexcept;

  [[nodiscard]] bool started() const noexcept { return started_; }

  // `now` is used for exactly one decision: a frame whose deadline has already
  // passed is refused as Late rather than stored. It is refused even though the
  // slot is free, because storing it would mean the consumer eventually plays
  // audio at the wrong point on its own timeline — a frame that is 60 ms late
  // is not 60 ms of audio you get to keep, it is 60 ms of audio that has been
  // overtaken.
  //
  // Lateness is tested twice over, in time and in sequence, and they are not
  // redundant: the cursor catches frames we already played over even when their
  // computed deadline was optimistic, and the deadline catches frames that are
  // still ahead of the cursor but can no longer make it. Both count as Late.
  Insert insert(const FrameInfo& info, ByteView payload, Micros now) noexcept;

  // ------------------------------------------------------------- consumer

  [[nodiscard]] std::uint32_t cursor() const noexcept { return cursor_; }

  // The frame at the cursor, if it has arrived. False leaves `out` untouched.
  [[nodiscard]] bool peek_next(Frame& out) const noexcept;

  // Any held frame within the window. The FEC path needs this: recovering the
  // frame at the cursor means reading the redundancy carried inside cursor + 1,
  // so the decoder has to look at a frame it is not yet ready to play.
  [[nodiscard]] bool peek(std::uint32_t sequence, Frame& out) const noexcept;

  [[nodiscard]] bool has_next() const noexcept;

  // Steps the cursor past the frame it currently points at, freeing the slot,
  // and reports whether anything was there. One call per playout tick.
  Step advance() noexcept;

  // Jumps the cursor forward to `sequence` and returns how many positions that
  // was. The catch-up primitive: when the consumer has fallen behind, walking
  // forward one 20 ms tick at a time never closes the distance.
  //
  // Every position jumped counts as a gap, because the consumer got no audio
  // for it. Frames that happened to be held in those positions are thrown away
  // and counted separately as discarded() — a frame we had and chose not to
  // play is a different event from one that never arrived, and lumping the two
  // together would make a catch-up look like packet loss.
  //
  // A `sequence` at or behind the cursor is a no-op, so this cannot rewind.
  std::size_t skip_to(std::uint32_t sequence) noexcept;

  // Discards held frames whose deadline has already passed. They can no longer
  // be played in the right place, and holding them only burns slots.
  //
  // Deliberately does NOT move the cursor. Whether to then catch up with
  // skip_to() or to accept a run of concealed frames is a policy decision, and
  // this class does not make those.
  std::size_t drop_expired(Micros now) noexcept;

  // ---------------------------------------------------------- observation

  // Frames currently stored.
  [[nodiscard]] std::size_t held() const noexcept { return held_; }

  // Frames of timeline between the cursor and the furthest frame held,
  // inclusive — the "buffer depth" the diagnostics screen reports. Larger than
  // held() whenever there are gaps, which is the point: depth is about latency,
  // held() is about occupancy, and a shallow buffer full of holes is a
  // different problem from a deep one.
  [[nodiscard]] std::size_t depth() const noexcept;

  [[nodiscard]] std::uint64_t accepted() const noexcept { return accepted_; }
  [[nodiscard]] std::uint64_t duplicates() const noexcept {
    return duplicates_;
  }
  [[nodiscard]] std::uint64_t late() const noexcept { return late_; }
  [[nodiscard]] std::uint64_t too_old() const noexcept { return too_old_; }
  [[nodiscard]] std::uint64_t too_far_ahead() const noexcept {
    return too_far_ahead_;
  }
  [[nodiscard]] std::uint64_t oversized() const noexcept { return oversized_; }

  // Arrived in time but was never pulled: the consumer fell behind. A different
  // diagnosis from late(), where the network was too slow, and the two are
  // routinely confused because both end up as a concealed frame.
  [[nodiscard]] std::uint64_t expired() const noexcept { return expired_; }

  // Held frames thrown away by skip_to() to catch up. Non-zero means the
  // consumer was late enough that audio it already had was not worth playing.
  [[nodiscard]] std::uint64_t discarded() const noexcept { return discarded_; }

  [[nodiscard]] std::uint64_t delivered() const noexcept { return delivered_; }
  [[nodiscard]] std::uint64_t gaps() const noexcept { return gaps_; }
  [[nodiscard]] std::uint64_t resyncs() const noexcept { return resyncs_; }

 private:
  struct Slot {
    FrameInfo info;
    std::uint16_t size = 0;
    bool occupied = false;
    std::array<std::byte, kMaxFramePayload> bytes{};
  };

  [[nodiscard]] static constexpr std::size_t index(
      std::uint32_t sequence) noexcept {
    return sequence & (kCapacity - 1);
  }

  // Clears the slot the cursor points at and moves the cursor on by one,
  // returning what was there.
  Step step_once() noexcept;

  std::array<Slot, kCapacity> slots_{};

  std::uint32_t cursor_ = 0;
  // Highest sequence accepted since the last reset or resync. Only meaningful
  // while held_ != 0, which is what depth() checks.
  std::uint32_t highest_ = 0;
  std::size_t held_ = 0;
  bool started_ = false;

  std::uint64_t accepted_ = 0;
  std::uint64_t duplicates_ = 0;
  std::uint64_t late_ = 0;
  std::uint64_t too_old_ = 0;
  std::uint64_t too_far_ahead_ = 0;
  std::uint64_t oversized_ = 0;
  std::uint64_t expired_ = 0;
  std::uint64_t discarded_ = 0;
  std::uint64_t delivered_ = 0;
  std::uint64_t gaps_ = 0;
  std::uint64_t resyncs_ = 0;
};

[[nodiscard]] const char* to_string(ReorderQueue::Insert v) noexcept;
[[nodiscard]] const char* to_string(ReorderQueue::Step v) noexcept;

}  // namespace radio::audio
