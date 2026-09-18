#include "radio/jitter_buffer.hpp"

#include <algorithm>

#include "radio/serial.hpp"

namespace radio::audio {

namespace {

// Upper bound on a single catch-up jump. It exists so that the arithmetic
// cannot be driven by an absurd `now` — a caller passing a wall-clock value by
// mistake would otherwise ask for a jump of several million frames. Anything
// approaching this is a dead stream, and the next arrival resyncs it anyway.
constexpr std::uint64_t kMaxCatchupJump = 1u << 20;

}  // namespace

const char* JitterBuffer::to_string(Push v) noexcept {
  switch (v) {
    case Push::Accepted:
      return "accepted";
    case Push::Duplicate:
      return "duplicate";
    case Push::Late:
      return "late";
    case Push::TooOld:
      return "too_old";
    case Push::Resynced:
      return "resynced";
    case Push::Foreign:
      return "foreign";
    case Push::Oversized:
      return "oversized";
    case Push::Closed:
      return "closed";
  }
  return "unknown";
}

const char* JitterBuffer::to_string(Source v) noexcept {
  switch (v) {
    case Source::Packet:
      return "packet";
    case Source::ForwardCorrection:
      return "fec";
    case Source::Concealment:
      return "concealment";
    case Source::Silence:
      return "silence";
  }
  return "unknown";
}

bool JitterBuffer::open(const Config& config) noexcept {
  close();
  if (!decoder_.open(config.decoder)) return false;

  config_ = config;
  frame_samples_ = config.decoder.frame_samples;
  frame_pcm_ = static_cast<std::size_t>(frame_samples_) *
               static_cast<std::size_t>(config.decoder.channels);

  // Derived from the decoder's own rate rather than assumed, then converted
  // back into the 48 kHz units the wire uses. For the configuration this
  // project actually ships both are 960; writing the conversion down is what
  // stops a 16 kHz decoder from advancing the timeline three times too slowly
  // while every counter still looks healthy.
  frame_us_ = static_cast<Micros>(frame_samples_) * 1'000'000u /
              config.decoder.sample_rate_hz;
  frame_ts_ = static_cast<std::uint32_t>(us_to_samples(frame_us_));

  // A zero frame duration would divide by zero in the catch-up arithmetic.
  // Decoder::open already rejects every frame size Opus does not accept, so
  // this is unreachable rather than defensive-by-habit -- but it is the kind
  // of unreachable that a future frame-size option makes reachable.
  if (frame_us_ == 0 || frame_ts_ == 0) {
    decoder_.close();
    return false;
  }
  return true;
}

void JitterBuffer::close() noexcept {
  decoder_.close();
  queue_.reset();
  config_ = Config{};
  frame_samples_ = kFrameSamples;
  frame_pcm_ = kFrameSamples;
  frame_ts_ = kFrameSamples;
  frame_us_ = 20'000;
  anchored_ = false;
  has_stream_ = false;
  stream_id_ = 0;
  last_arrival_us_ = 0;
  anchor_ts_ = 0;
  anchor_local_us_ = 0;
  playout_ts_ = 0;
  conceal_run_ = 0;

  from_packet_ = 0;
  fec_recovered_ = 0;
  concealed_ = 0;
  silence_ = 0;
  muted_ = 0;
  catchups_ = 0;
  catchup_frames_ = 0;
  anchors_ = 0;
  foreign_ = 0;
  decode_errors_ = 0;
}

void JitterBuffer::reset_stream() noexcept {
  queue_.reset();
  anchored_ = false;
  has_stream_ = false;
  conceal_run_ = 0;
  // The decoder's history belongs to the stream that just ended. Extrapolating
  // the next talkspurt from the tail of the previous speaker's voice is worse
  // than starting cold, and costs nothing to avoid.
  (void)decoder_.reset_state();
}

Micros JitterBuffer::deadline_of(std::uint32_t timestamp) const noexcept {
  const std::int32_t delta = serial::distance(anchor_ts_, timestamp);
  if (delta >= 0) {
    return anchor_local_us_ + samples_to_us(static_cast<std::uint64_t>(delta));
  }
  // A frame that sits before the anchor — a reordered arrival from just before
  // a talkspurt boundary. Its moment is in the past; saturating at zero says
  // "due now" without underflowing into a deadline 580,000 years away, which
  // is what unsigned subtraction would have produced.
  const auto back =
      static_cast<std::uint64_t>(-static_cast<std::int64_t>(delta));
  const Micros back_us = samples_to_us(back);
  return back_us > anchor_local_us_ ? 0 : anchor_local_us_ - back_us;
}

Micros JitterBuffer::next_deadline_us() const noexcept {
  return anchored_ ? deadline_of(playout_ts_) : 0;
}

void JitterBuffer::anchor(std::uint32_t timestamp, Micros arrival_us) noexcept {
  anchor_ts_ = timestamp;
  // The single line that sets the latency of the whole pipeline: this frame is
  // due target_delay_us after it arrived, and every other frame is placed
  // relative to it by its timestamp.
  anchor_local_us_ = arrival_us + config_.target_delay_us;
  anchored_ = true;
  ++anchors_;
}

JitterBuffer::Push JitterBuffer::push(const proto::MediaPacket& packet,
                                      Micros arrival_us) noexcept {
  if (!decoder_.is_open()) return Push::Closed;

  const proto::MediaHeader& header = packet.header;

  if (!has_stream_) {
    has_stream_ = true;
    stream_id_ = header.stream_id;
  } else if (header.stream_id != stream_id_) {
    // Spec §3.2: a straggler from the talkspurt that just ended arrives on a
    // stream id we are no longer playing. Tearing down the live stream for it
    // would turn one late packet into a whole dropped talkspurt, so a foreign
    // id only takes over if it announces itself, or if nothing has arrived on
    // the current stream for long enough that there is nothing left to
    // protect.
    const bool announced = header.talkspurt_start();
    const bool idle = arrival_us > last_arrival_us_ &&
                      arrival_us - last_arrival_us_ >= config_.stream_idle_us;
    if (!announced && !idle) {
      ++foreign_;
      return Push::Foreign;
    }
    reset_stream();
    has_stream_ = true;
    stream_id_ = header.stream_id;
  }

  last_arrival_us_ = arrival_us;

  if (!anchored_) {
    anchor(header.timestamp, arrival_us);
    queue_.start(header.sequence);
    playout_ts_ = header.timestamp;
  } else if (header.talkspurt_start()) {
    if (serial::precedes_or_equal(queue_.cursor(), header.sequence)) {
      // The normal case: the flag arrives one buffer-depth ahead of its own
      // playout, so the mapping changes while the cursor is still walking
      // through the tail of what came before. Only the mapping moves.
      anchor(header.timestamp, arrival_us);
    } else {
      // We already ran past this sequence — a DTX gap long enough that the
      // cursor concealed and then muted its way through it. There is nothing
      // behind the cursor worth keeping, so the timeline restarts here.
      queue_.resync(header.sequence);
      anchor(header.timestamp, arrival_us);
      playout_ts_ = header.timestamp;
      conceal_run_ = 0;
      (void)decoder_.reset_state();
    }
  }

  ReorderQueue::FrameInfo info;
  info.header = header;
  info.arrival_us = arrival_us;
  info.deadline_us = deadline_of(header.timestamp);

  ReorderQueue::Insert result = queue_.insert(info, packet.payload, arrival_us);

  if (result == ReorderQueue::Insert::TooFarAhead) {
    // The mechanism refuses to guess; deciding what a 64-frame jump means is
    // this layer's job. On a LAN it is a sender restart or a stall of over a
    // second, and in both cases the old timeline is worthless.
    queue_.resync(header.sequence);
    anchor(header.timestamp, arrival_us);
    playout_ts_ = header.timestamp;
    conceal_run_ = 0;
    (void)decoder_.reset_state();
    info.deadline_us = deadline_of(header.timestamp);
    result = queue_.insert(info, packet.payload, arrival_us);
    return result == ReorderQueue::Insert::Accepted ? Push::Resynced
                                                    : Push::Oversized;
  }

  switch (result) {
    case ReorderQueue::Insert::Accepted:
      return Push::Accepted;
    case ReorderQueue::Insert::Duplicate:
      return Push::Duplicate;
    case ReorderQueue::Insert::Late:
      return Push::Late;
    case ReorderQueue::Insert::TooOld:
      return Push::TooOld;
    case ReorderQueue::Insert::Oversized:
      return Push::Oversized;
    case ReorderQueue::Insert::TooFarAhead:
      break;  // handled above
  }
  return Push::Resynced;
}

void JitterBuffer::write_silence(std::span<std::int16_t> pcm) const noexcept {
  std::fill_n(pcm.data(), frame_pcm_, std::int16_t{0});
}

bool JitterBuffer::peek_and_align(ReorderQueue::Frame& out) noexcept {
  if (!queue_.peek_next(out)) return false;
  // The sender skipped forward on its own timeline without skipping sequence
  // numbers: a DTX gap short enough that the packet announcing it arrived
  // before we walked over its slot. Moving the playout position onto the real
  // timestamp is what makes the deadline check below wait out the silence
  // instead of playing the frame early.
  if (serial::precedes(playout_ts_, out.info.header.timestamp)) {
    playout_ts_ = out.info.header.timestamp;
  }
  return true;
}

JitterBuffer::Pulled JitterBuffer::pull(std::span<std::int16_t> pcm,
                                        Micros now) noexcept {
  Pulled out;
  out.sequence = queue_.cursor();
  out.timestamp = playout_ts_;

  // OPUS_BUFFER_TOO_SMALL, spelled out so this file does not need <opus.h>.
  // Checked here as well as inside the decoder because the silence paths never
  // reach the decoder and would otherwise write past the end of the span.
  if (pcm.size() < frame_pcm_ || !decoder_.is_open()) {
    out.error = -2;
    return out;
  }

  out.samples = frame_pcm_;

  // Nothing has ever arrived, so there is no timeline to be on. The device is
  // still asking every 20 ms and something has to go to the speaker.
  if (!anchored_) {
    write_silence(pcm);
    ++silence_;
    return out;
  }

  ReorderQueue::Frame frame;
  bool have = peek_and_align(frame);
  out.timestamp = playout_ts_;

  // Pre-roll and DTX gaps are the same condition seen twice: the frame at the
  // cursor is not due yet. Emit silence and do NOT advance — the cursor waits
  // on the timeline, and this is where target_delay_us actually becomes delay.
  const Micros due = deadline_of(playout_ts_);
  if (now < due) {
    write_silence(pcm);
    ++silence_;
    return out;
  }

  // Far enough past the deadline that walking cannot recover: a stalled audio
  // thread, a suspended process, or a benchmark that stopped calling pull().
  // Jump the cursor to where the timeline says we should be. Every frame
  // jumped is counted as a gap by the queue, and any frame that had arrived in
  // that range is counted as discarded, so a catch-up never masquerades as
  // packet loss.
  const std::uint64_t behind = (now - due) / frame_us_;
  if (behind > config_.max_catchup_frames) {
    const std::uint64_t jump = std::min(behind, kMaxCatchupJump);
    queue_.skip_to(queue_.cursor() + static_cast<std::uint32_t>(jump));
    playout_ts_ += static_cast<std::uint32_t>(jump) * frame_ts_;
    ++catchups_;
    catchup_frames_ += jump;
    conceal_run_ = 0;
    (void)decoder_.reset_state();

    out.sequence = queue_.cursor();
    have = peek_and_align(frame);
    out.timestamp = playout_ts_;
  }

  if (have) {
    const Decoder::Result r = decoder_.decode(frame.payload, pcm);
    out.sequence = frame.info.header.sequence;
    out.timestamp = frame.info.header.timestamp;
    out.error = r.error;
    if (r.ok()) {
      out.source = Source::Packet;
      out.samples = r.samples;
      ++from_packet_;
      conceal_run_ = 0;
    } else {
      // A decoder error still owes the device a frame. Silence is the honest
      // filler: concealing would extrapolate from history that the failed
      // decode may just have corrupted.
      write_silence(pcm);
      ++decode_errors_;
      ++silence_;
    }
    queue_.advance();
    playout_ts_ += frame_ts_;
    return out;
  }

  // Nothing at the cursor. Three ways to fill the hole, in order of how much
  // of the original frame each one preserves.
  bool filled = false;

  if (config_.fec) {
    ReorderQueue::Frame next;
    // The redundancy for frame N travels inside packet N+1, so recovering the
    // frame we want means reading a frame we are not ready to play. The
    // redundancy check is not optional: recover_previous() falls back to
    // concealment and reports success, so counting every successful call as an
    // FEC recovery would inflate the one number M1 exists to measure.
    if (queue_.peek(queue_.cursor() + 1, next) &&
        Decoder::packet_has_redundancy(next.payload)) {
      const Decoder::Result r = decoder_.recover_previous(next.payload, pcm);
      if (r.ok()) {
        out.source = Source::ForwardCorrection;
        out.samples = r.samples;
        ++fec_recovered_;
        conceal_run_ = 0;
        filled = true;
      }
    }
  }

  if (!filled) {
    if (conceal_run_ >= config_.max_conceal_run) {
      // Past this point concealment is a synthetic drone that sounds worse
      // than nothing, so the buffer stops pretending. reset_state() runs once,
      // on the transition, so that whatever arrives next decodes from a clean
      // history rather than from the tail of an extrapolation.
      if (conceal_run_ == config_.max_conceal_run) {
        ++muted_;
        (void)decoder_.reset_state();
      }
      write_silence(pcm);
      ++silence_;
      ++conceal_run_;
      out.source = Source::Silence;
    } else {
      const Decoder::Result r = decoder_.conceal(pcm);
      if (r.ok()) {
        out.source = Source::Concealment;
        out.samples = r.samples;
        ++concealed_;
      } else {
        write_silence(pcm);
        ++decode_errors_;
        ++silence_;
        out.error = r.error;
      }
      ++conceal_run_;
    }
  }

  queue_.advance();
  playout_ts_ += frame_ts_;
  return out;
}

}  // namespace radio::audio
