// Opus encoder and decoder wrappers.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "radio/bytes.hpp"
#include "radio/clock.hpp"

// Opaque forward declarations, so <opus.h> stays out of this interface.
//
// Both are `typedef struct OpusEncoder OpusEncoder;` upstream, so a pointer is
// all that is needed here. Keeping the Opus headers in the .cpp means the JNI
// bridge and the future Swift-facing C ABI do not inherit them, for the same
// reason net::Endpoint does not expose sockaddr.
struct OpusEncoder;
struct OpusDecoder;

namespace radio::audio {

// Opus accepts only these frame durations. At 48 kHz:
//   2.5 ms = 120, 5 ms = 240, 10 ms = 480, 20 ms = 960, 40 ms = 1920,
//   60 ms = 2880 samples.
// Anything else is rejected by the library, so it is rejected here with a
// clearer error than OPUS_BAD_ARG.
[[nodiscard]] bool is_valid_frame_samples(std::uint32_t sample_rate_hz,
                                          std::uint32_t frame_samples) noexcept;

// Opus error code as text, without the caller needing <opus.h>.
[[nodiscard]] const char* error_string(int opus_error) noexcept;

struct EncoderConfig {
  std::uint32_t sample_rate_hz = kSampleRateHz;
  int channels = 1;
  std::uint32_t frame_samples = kFrameSamples;  // 960 = 20 ms at 48 kHz

  std::int32_t bitrate_bps = 32'000;

  // 0 cheapest, 10 best quality. 5 is a reasonable default for voice on a
  // phone; the encode cost is roughly linear in this and it is one of the few
  // knobs that trades CPU for quality directly.
  int complexity = 5;

  // In-band forward error correction.
  //
  // Enabling this alone produces NO redundancy. Opus only emits FEC data when
  // it also believes packets are being lost, which it learns from
  // expected_loss_percent. `inband_fec = true` with `expected_loss_percent = 0`
  // is a silent no-op, and a common way to conclude that FEC "does not work".
  //
  // open() therefore rejects that combination outright rather than accepting a
  // configuration that cannot do what it claims.
  bool inband_fec = false;

  // What the encoder should assume about loss, 0-100. Drives how much
  // redundancy FEC adds and how conservatively the encoder codes. M4 adjusts
  // this from measured loss.
  int expected_loss_percent = 0;

  // Discontinuous transmission: emit 1-byte packets during silence instead of
  // full frames. Saves bandwidth, and requires the receiver to handle timestamp
  // gaps (which is why the wire format carries a timestamp as well as a
  // sequence number).
  bool dtx = false;

  // Variable bitrate. On for voice: it spends bits where they matter and is
  // what makes FEC affordable at a given average rate.
  bool vbr = true;
};

// Owns an OpusEncoder.
//
// Move-only, like every other resource handle here: two copies would both call
// opus_encoder_destroy on the same state.
//
// opus_encoder_create allocates once, at open(). opus_encode itself does not
// allocate — Opus is built for embedded use and keeps its scratch inside the
// state — so the per-frame path is allocation-free, which is what the audio
// deadline requires. (opus_encoder_get_size and opus_encoder_init would allow
// placing the state in caller-provided memory and avoid even the one-time
// malloc; that is not needed while open() happens at start-up.)
class Encoder {
 public:
  Encoder() noexcept = default;
  ~Encoder();

  Encoder(Encoder&& other) noexcept;
  Encoder& operator=(Encoder&& other) noexcept;
  Encoder(const Encoder&) = delete;
  Encoder& operator=(const Encoder&) = delete;

  [[nodiscard]] bool open(const EncoderConfig& config) noexcept;
  void close() noexcept;

  [[nodiscard]] bool is_open() const noexcept { return state_ != nullptr; }
  [[nodiscard]] int last_error() const noexcept { return last_error_; }
  [[nodiscard]] const EncoderConfig& config() const noexcept { return config_; }

  struct Result {
    std::size_t bytes = 0;

    // True when DTX decided this frame is silence and need not be sent at all.
    // `bytes` is then whatever Opus produced (1 byte or less) and the caller
    // should transmit nothing.
    bool dtx_skipped = false;

    int error = 0;  // OPUS_OK is 0

    [[nodiscard]] bool ok() const noexcept { return error == 0; }
  };

  // Encodes exactly config().frame_samples of interleaved PCM.
  //
  // A short or long input is rejected rather than padded: silently encoding the
  // wrong duration would desynchronise the timestamp on the wire from the audio
  // actually sent, and that is very hard to see afterwards.
  [[nodiscard]] Result encode(std::span<const std::int16_t> pcm,
                              ByteSpan out) noexcept;

  // Runtime adjustment, for the adaptive controller in M4.
  [[nodiscard]] bool set_bitrate(std::int32_t bitrate_bps) noexcept;
  [[nodiscard]] bool set_expected_loss_percent(int percent) noexcept;
  [[nodiscard]] bool set_inband_fec(bool enabled) noexcept;
  [[nodiscard]] bool set_complexity(int complexity) noexcept;

  // Read back from the library rather than from our own copy of the request.
  // Opus clamps and reinterprets several settings, so what it is actually doing
  // is not always what was asked for -- and for a benchmark, the difference
  // matters.
  [[nodiscard]] std::int32_t query_bitrate_bps() const noexcept;
  [[nodiscard]] bool query_inband_fec() const noexcept;
  [[nodiscard]] int query_expected_loss_percent() const noexcept;

 private:
  OpusEncoder* state_ = nullptr;
  EncoderConfig config_{};
  int last_error_ = 0;
};

struct DecoderConfig {
  std::uint32_t sample_rate_hz = kSampleRateHz;
  int channels = 1;
  std::uint32_t frame_samples = kFrameSamples;
};

// Owns an OpusDecoder.
//
// Three ways to produce a frame, deliberately separate methods rather than one
// function with flags, because they mean different things and the metrics have
// to distinguish them.
class Decoder {
 public:
  Decoder() noexcept = default;
  ~Decoder();

  Decoder(Decoder&& other) noexcept;
  Decoder& operator=(Decoder&& other) noexcept;
  Decoder(const Decoder&) = delete;
  Decoder& operator=(const Decoder&) = delete;

  [[nodiscard]] bool open(const DecoderConfig& config) noexcept;
  void close() noexcept;

  [[nodiscard]] bool is_open() const noexcept { return state_ != nullptr; }
  [[nodiscard]] int last_error() const noexcept { return last_error_; }
  [[nodiscard]] const DecoderConfig& config() const noexcept { return config_; }

  // Where a frame of audio came from. Counted separately in the metrics: a call
  // that sounds fine because FEC recovered it is a very different situation
  // from one that sounds fine because nothing was lost.
  enum class Source : std::uint8_t {
    Packet,             // decoded normally from a received packet
    Concealment,        // invented by the codec; nothing was available
    ForwardCorrection,  // reconstructed from redundancy in the NEXT packet
  };

  [[nodiscard]] static const char* to_string(Source source) noexcept;

  struct Result {
    std::size_t samples = 0;
    Source source = Source::Packet;
    int error = 0;
    [[nodiscard]] bool ok() const noexcept { return error == 0; }
  };

  // Normal path.
  [[nodiscard]] Result decode(ByteView packet,
                              std::span<std::int16_t> pcm) noexcept;

  // Packet loss concealment: nothing arrived, so the codec extrapolates from
  // what it has already decoded. Convincing for one frame, obviously wrong by
  // about the third -- which is why burst loss matters more than the loss rate.
  [[nodiscard]] Result conceal(std::span<std::int16_t> pcm) noexcept;

  // Reconstructs the frame BEFORE `next_packet` from the redundancy inside it.
  //
  // This is the part of Opus FEC that surprises people. The redundancy for
  // frame N-1 travels inside frame N, so recovering a loss means decoding the
  // next packet twice:
  //
  //   frame 41 lost, packet 42 arrives:
  //     recover_previous(packet42, pcm)  -> frame 41
  //     decode(packet42, pcm)            -> frame 42
  //
  // It also explains why FEC repairs isolated losses and not bursts: packet
  // 42's copy of frame 41 is worth nothing if 42 was lost too.
  //
  // IMPORTANT, and measured rather than assumed: when `next_packet` carries no
  // redundancy this does NOT fail. Opus silently falls back to concealment and
  // reports success, so the return value cannot distinguish a genuine recovery
  // from a concealed frame dressed as one.
  //
  // That matters for metrics. Counting every successful call as "recovered by
  // FEC" would inflate the figure with frames that were actually concealed, and
  // knowing which is happening is the entire reason those two are counted
  // separately.
  //
  // Check packet_has_redundancy() first. Measured against the frame that would
  // have been decoded had nothing been lost, mean absolute sample error on a
  // signal of amplitude 12000:
  //
  //     concealment    2522
  //     FEC recovery     27       about 90x closer
  //
  // so the distinction is not academic.
  [[nodiscard]] Result recover_previous(ByteView next_packet,
                                        std::span<std::int16_t> pcm) noexcept;

  // Whether a packet actually carries redundancy for the frame before it.
  //
  // Opus emits it only when in-band FEC is enabled AND the encoder believes
  // packets are being lost AND the bitrate can afford it. Measured at 48 kHz
  // mono, 20 ms frames, over 30 frames:
  //
  //     32 kbps, expected loss  0%   ->   0/30 packets carry redundancy
  //     32 kbps, expected loss 20%   ->  25/30
  //     16 kbps, expected loss 20%   ->   0/30   (rate too low to afford it)
  //
  // The first row is why EncoderConfig rejects FEC with a zero loss estimate.
  // The third is why an adaptive controller cannot assume FEC remains available
  // at every bitrate it might choose.
  [[nodiscard]] static bool packet_has_redundancy(ByteView packet) noexcept;

  // Resets the decoder's internal history. Used at a talkspurt boundary, where
  // extrapolating from audio that ended two seconds ago would be worse than
  // starting clean.
  [[nodiscard]] bool reset_state() noexcept;

  // How many samples a packet contains, without decoding it. Needed by the
  // jitter buffer to know how much timeline a packet covers before committing
  // to it.
  [[nodiscard]] static std::size_t packet_samples(
      ByteView packet, std::uint32_t sample_rate_hz) noexcept;

 private:
  OpusDecoder* state_ = nullptr;
  DecoderConfig config_{};
  int last_error_ = 0;
};

}  // namespace radio::audio
