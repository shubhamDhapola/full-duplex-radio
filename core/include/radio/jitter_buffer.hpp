// Playout policy: turns an irregular stream of arrivals into one frame of PCM
// every 20 ms, on the consumer's clock.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "radio/clock.hpp"
#include "radio/opus_codec.hpp"
#include "radio/proto.hpp"
#include "radio/reorder_queue.hpp"

namespace radio::audio {

// WHERE THIS SITS
//
//   network thread            SpscRing            consumer thread
//   ------------------------------------------------------------------
//   recvfrom, parse    ->    whole datagrams  ->  push()
//                                                 pull()  -> PCM -> device
//
// ReorderQueue is the mechanism below this: storage, ordering, and the
// classification of every arrival. This class is the policy on top of it —
// how deep to buffer, where a frame sits on the local timeline, when to
// re-anchor, and whether a hole should be concealed or recovered. Everything
// here is a decision; nothing here is a data structure.
//
// THE PULL MODEL, AND WHY IT IS NOT A DETAIL
//
// The obvious design is a push model: a timer fires every 20 ms, decodes a
// frame and writes it somewhere for the audio device to collect. It is wrong,
// and the reason is that it creates a second clock.
//
// The audio device is a hardware oscillator asking for exactly 960 samples at
// its own rate, which is 48 kHz only nominally — the real crystal is off by
// tens of parts per million. A timer thread is driven by the system clock,
// which is off by a different amount. The two diverge by a few milliseconds an
// hour, so the decoder eventually produces frames slightly faster or slower
// than the device consumes them, and the buffer between them either grows
// without bound or empties. That is a real bug in real products, and it is
// invisible for the first ten minutes of every test.
//
// In the pull model the audio callback calls pull() directly, and decoding
// happens inside that call. There is no second clock to drift against, because
// the only clock that advances the playout timeline IS the device asking for
// audio. It costs an Opus decode inside the callback — measured at well under
// 1 ms for a 20 ms frame, against a budget of 20 — and buys the removal of an
// entire class of bug.
//
// The remaining clock difference, between the device and the *sender's*
// crystal, is real and is M4's problem. This class makes it observable
// (`depth()` trending in one direction is exactly that drift) rather than
// hiding it inside a queue that silently grows.
//
// TWO NUMBERS DESCRIBE THE TIMELINE, AND THAT IS ALL
//
// A media header carries a 48 kHz `timestamp` in the sender's units, which say
// nothing about when we should play it. The mapping to our clock is one affine
// function, held as a single anchor pair:
//
//   deadline(ts) = anchor_local_us + (ts - anchor_ts) converted to microseconds
//
// The anchor is set when a stream starts, and reset at a talkspurt boundary
// where spec §3.1 asks for it. `target_delay_us` enters in exactly one place:
// the anchor is placed that far *after* the arrival that set it. Everything
// people describe as "the buffer holds 60 ms" falls out of that one offset —
// there is no separate pre-roll counter and no separate fill state, because
// pre-roll is just "now has not reached the first frame's deadline yet".
//
// A short DTX gap then handles itself. The sequence advances by one while the
// timestamp jumps by the whole silence (spec §3.3), so once that packet is in
// the queue its deadline sits far in the future and pull() emits silence until
// then. No code was written for that case; it is what a timestamp-driven
// timeline does.
//
// A long one cannot, and it is worth being clear about why: until the
// resumption packet arrives, a DTX gap and a dead sender are the same
// observation. The buffer therefore treats it as loss — conceals, gives up
// after max_conceal_run and emits silence — and the TALKSPURT_START on
// resumption puts the timeline back. Guessing instead, by holding the cursor
// whenever the queue is empty, would make every real loss add permanent
// latency.
//
// THREADING: NONE, by the same argument as ReorderQueue.
//
// The consumer thread owns this object, the queue inside it and the decoder
// inside that. The network thread only fills an SpscRing. Do not call push()
// and pull() from different threads.
class JitterBuffer {
 public:
  struct Config {
    DecoderConfig decoder{};

    // How far behind real time the playout timeline runs. This is *the*
    // latency knob: every millisecond here is a millisecond of mouth-to-ear
    // delay, and every millisecond removed is packets that arrive after their
    // moment and count as late.
    //
    // 60 ms is three frames — enough to absorb one late packet and reorder a
    // swapped pair, which is what a LAN actually does. M4 makes it adaptive;
    // until there is a measurement to adapt from, a fixed value that can be
    // compared against is worth more than a controller that cannot be.
    Micros target_delay_us = 60'000;

    // Attempt FEC recovery of a missing frame from the redundancy in the next
    // one. Off is a legitimate configuration: the benchmark matrix runs the
    // same impairment pattern with it on and off, and the difference is the
    // whole acceptance criterion for M1.
    bool fec = true;

    // Consecutive concealed frames before the buffer gives up and emits
    // silence instead.
    //
    // Opus concealment extrapolates from what it last decoded. One frame is
    // convincing, two is acceptable, and by the fifth it is a synthetic drone
    // that sounds worse than nothing and is actively misleading to listen to
    // in a benchmark. 5 frames is 100 ms.
    std::uint32_t max_conceal_run = 5;

    // How far behind the timeline the consumer may fall before the buffer
    // jumps forward instead of walking. Below this it walks, concealing as it
    // goes, which keeps the audio continuous. Above it, walking at one frame
    // per 20 ms tick never closes the distance — playing 400 ms of stale audio
    // to recover from a 400 ms stall just moves the stall.
    std::uint32_t max_catchup_frames = 5;

    // A datagram bearing a different stream_id is adopted immediately if it
    // announces itself with TALKSPURT_START (spec §3.2). Without that flag it
    // is adopted only once the current stream has been quiet this long, so a
    // straggler from the previous talkspurt cannot tear down the current one.
    Micros stream_idle_us = 200'000;
  };

  // What push() did with a datagram. Distinct from ReorderQueue::Insert
  // because two of the mechanism's answers become decisions here: TooFarAhead
  // turns into a resync, and a foreign stream_id is a concept the queue below
  // has never heard of.
  enum class Push : std::uint8_t {
    Accepted,
    Duplicate,
    Late,       // arrived after its playout moment: the buffer is too shallow
    TooOld,     // so far behind that late-versus-stray is unknowable
    Resynced,   // beyond the window; the timeline was re-anchored onto it
    Foreign,    // another stream, and not entitled to take over yet
    Oversized,  // payload larger than a queue slot
    Closed,     // no decoder open
  };

  // Where one frame of PCM came from. Counted separately because "it sounded
  // fine" has three completely different causes and only one of them means the
  // network behaved.
  enum class Source : std::uint8_t {
    Packet,             // decoded from a datagram that arrived in time
    ForwardCorrection,  // rebuilt from redundancy inside the NEXT datagram
    Concealment,        // invented by the codec; nothing was available
    Silence,            // deliberately nothing: pre-roll, DTX gap, or muted
  };

  struct Pulled {
    Source source = Source::Silence;

    // The timeline position this frame occupies, whether or not a datagram
    // ever turned up for it. Sequence is the queue cursor; timestamp is the
    // sender-clock position, projected forward when the frame is missing.
    std::uint32_t sequence = 0;
    std::uint32_t timestamp = 0;

    std::size_t samples = 0;
    int error = 0;  // OPUS_OK is 0

    [[nodiscard]] bool ok() const noexcept { return error == 0; }
  };

  [[nodiscard]] static const char* to_string(Push v) noexcept;
  [[nodiscard]] static const char* to_string(Source v) noexcept;

  [[nodiscard]] bool open(const Config& config) noexcept;
  void close() noexcept;
  [[nodiscard]] bool is_open() const noexcept { return decoder_.is_open(); }

  // Drops the timeline and all held frames, and resets the decoder's history.
  // Counters survive; they describe the session, not the stream.
  void reset_stream() noexcept;

  [[nodiscard]] const Config& config() const noexcept { return config_; }

  // `arrival_us` is the socket's own timestamp for the datagram, not the time
  // push() happens to run. It is the instant the packet is measured against
  // its deadline, so taking it later would credit the network with time the
  // application spent.
  Push push(const proto::MediaPacket& packet, Micros arrival_us) noexcept;

  // Produces exactly one frame. `pcm` must hold at least frame_samples; extra
  // space is left untouched.
  //
  // Always writes a full frame, even on a decoder error, because the audio
  // device has no error path — whatever this returns is going to the speaker,
  // and leaving the buffer as it was found means playing whatever the previous
  // tick left there.
  Pulled pull(std::span<std::int16_t> pcm, Micros now) noexcept;

  // --------------------------------------------------------- observation

  // The queue underneath, for its arrival classification counters: late(),
  // duplicates(), too_old(), gaps(), depth(). They are not mirrored here,
  // because two copies of a counter eventually disagree.
  [[nodiscard]] const ReorderQueue& queue() const noexcept { return queue_; }
  [[nodiscard]] const Decoder& decoder() const noexcept { return decoder_; }

  [[nodiscard]] bool anchored() const noexcept { return anchored_; }
  [[nodiscard]] std::uint32_t stream_id() const noexcept { return stream_id_; }

  // The local time the frame currently at the cursor is due. Meaningless
  // before the first arrival.
  [[nodiscard]] Micros next_deadline_us() const noexcept;

  [[nodiscard]] std::uint64_t from_packet() const noexcept {
    return from_packet_;
  }
  [[nodiscard]] std::uint64_t fec_recovered() const noexcept {
    return fec_recovered_;
  }
  [[nodiscard]] std::uint64_t concealed() const noexcept { return concealed_; }

  // Frames where the buffer had nothing to play and chose not to invent
  // anything: before the first packet, inside a DTX gap, past a
  // TALKSPURT_END, or past max_conceal_run.
  [[nodiscard]] std::uint64_t silence() const noexcept { return silence_; }

  // Concealment runs that hit max_conceal_run and were cut off. Non-zero means
  // the sender went away for more than 100 ms, which is a transport problem
  // and not a codec one.
  [[nodiscard]] std::uint64_t muted() const noexcept { return muted_; }

  [[nodiscard]] std::uint64_t catchups() const noexcept { return catchups_; }
  [[nodiscard]] std::uint64_t catchup_frames() const noexcept {
    return catchup_frames_;
  }
  // True once a TALKSPURT_END has been seen for the current stream. The
  // sender said it had finished, so a hole past that sequence is the end of
  // speech rather than a loss, and is filled with silence instead of
  // concealment (spec 3.1).
  //
  // Worth stating why this is not cosmetic: without it, every benchmark run
  // ends with a handful of concealed frames while the buffer drains, and a
  // zero-loss scenario reports concealment. A counter that is non-zero when
  // nothing went wrong is a counter nobody reads.
  //
  // The flag is advisory and may be lost, in which case the tail is concealed
  // as before -- which is the correct fallback, because a lost end flag is
  // indistinguishable from a sender that stopped abruptly.
  [[nodiscard]] bool talkspurt_ended() const noexcept { return has_end_; }

  [[nodiscard]] std::uint64_t anchors() const noexcept { return anchors_; }
  [[nodiscard]] std::uint64_t foreign() const noexcept { return foreign_; }
  [[nodiscard]] std::uint64_t decode_errors() const noexcept {
    return decode_errors_;
  }

 private:
  // Maps a sender timestamp onto the local clock through the current anchor.
  [[nodiscard]] Micros deadline_of(std::uint32_t timestamp) const noexcept;

  // Places the anchor so that `timestamp` is due target_delay_us after
  // `arrival_us`, and puts the playout position on it.
  void anchor(std::uint32_t timestamp, Micros arrival_us) noexcept;

  // Reads the frame at the cursor if it is there, and pulls the playout
  // position forward onto its timestamp when the sender skipped ahead — a DTX
  // gap, whose whole effect is that the next deadline is far away.
  [[nodiscard]] bool peek_and_align(ReorderQueue::Frame& out) noexcept;

  void write_silence(std::span<std::int16_t> pcm) const noexcept;

  Config config_{};
  ReorderQueue queue_{};
  Decoder decoder_{};

  // Three different units for the same 20 ms, and conflating any two of them
  // is a bug that sounds like drift:
  //   frame_samples_  PCM samples the decoder produces, at ITS rate
  //   frame_pcm_      those samples times the channel count, i.e. buffer size
  //   frame_ts_       wire timestamp units, always 48 kHz (spec §3.3)
  std::uint32_t frame_samples_ = kFrameSamples;
  std::size_t frame_pcm_ = kFrameSamples;
  std::uint32_t frame_ts_ = kFrameSamples;
  Micros frame_us_ = 20'000;

  bool anchored_ = false;
  bool has_stream_ = false;
  std::uint32_t stream_id_ = 0;
  Micros last_arrival_us_ = 0;

  std::uint32_t anchor_ts_ = 0;
  Micros anchor_local_us_ = 0;

  // Sender-clock position of the frame the cursor points at. Projected forward
  // by one frame per tick, and snapped onto a real timestamp whenever one
  // arrives, so a lost frame does not stop the timeline from advancing.
  std::uint32_t playout_ts_ = 0;

  std::uint32_t conceal_run_ = 0;

  // The sequence the sender said was its last, if it said so.
  bool has_end_ = false;
  std::uint32_t end_sequence_ = 0;

  std::uint64_t from_packet_ = 0;
  std::uint64_t fec_recovered_ = 0;
  std::uint64_t concealed_ = 0;
  std::uint64_t silence_ = 0;
  std::uint64_t muted_ = 0;
  std::uint64_t catchups_ = 0;
  std::uint64_t catchup_frames_ = 0;
  std::uint64_t anchors_ = 0;
  std::uint64_t foreign_ = 0;
  std::uint64_t decode_errors_ = 0;
};

}  // namespace radio::audio
