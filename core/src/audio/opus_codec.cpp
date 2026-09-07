#include "radio/opus_codec.hpp"

#include <opus.h>

#include <utility>

namespace radio::audio {
namespace {

// Frame durations Opus accepts, in tenths of a millisecond so 2.5 ms is exact.
constexpr int kValidFrameTenthsMs[] = {25, 50, 100, 200, 400, 600};

}  // namespace

bool is_valid_frame_samples(std::uint32_t sample_rate_hz,
                            std::uint32_t frame_samples) noexcept {
  if (sample_rate_hz == 0 || frame_samples == 0) return false;
  for (const int tenths : kValidFrameTenthsMs) {
    // samples = rate * tenths / 10000, computed without floating point so the
    // comparison is exact.
    const std::uint64_t expected = static_cast<std::uint64_t>(sample_rate_hz) *
                                   static_cast<std::uint64_t>(tenths) /
                                   10'000ull;
    if (expected == frame_samples) return true;
  }
  return false;
}

const char* error_string(int opus_error) noexcept {
  return opus_strerror(opus_error);
}

// ------------------------------------------------------------------ Encoder

Encoder::~Encoder() { close(); }

Encoder::Encoder(Encoder&& other) noexcept
    : state_(other.state_),
      config_(other.config_),
      last_error_(other.last_error_) {
  other.state_ = nullptr;  // or both destructors free the same state
}

Encoder& Encoder::operator=(Encoder&& other) noexcept {
  if (this != &other) {
    close();
    state_ = other.state_;
    config_ = other.config_;
    last_error_ = other.last_error_;
    other.state_ = nullptr;
  }
  return *this;
}

void Encoder::close() noexcept {
  if (state_ != nullptr) {
    opus_encoder_destroy(state_);
    state_ = nullptr;
  }
}

bool Encoder::open(const EncoderConfig& config) noexcept {
  close();

  if (config.channels != 1 && config.channels != 2) {
    last_error_ = OPUS_BAD_ARG;
    return false;
  }
  if (!is_valid_frame_samples(config.sample_rate_hz, config.frame_samples)) {
    last_error_ = OPUS_BAD_ARG;
    return false;
  }
  // Rejected rather than accepted-and-ignored. Opus emits no redundancy at all
  // when it believes loss is zero, so this combination is a configuration that
  // cannot do what it says, and accepting it is how people conclude FEC does
  // not work.
  if (config.inband_fec && config.expected_loss_percent <= 0) {
    last_error_ = OPUS_BAD_ARG;
    return false;
  }
  if (config.expected_loss_percent < 0 || config.expected_loss_percent > 100) {
    last_error_ = OPUS_BAD_ARG;
    return false;
  }
  if (config.complexity < 0 || config.complexity > 10) {
    last_error_ = OPUS_BAD_ARG;
    return false;
  }

  int error = OPUS_OK;
  state_ = opus_encoder_create(static_cast<opus_int32>(config.sample_rate_hz),
                               config.channels, OPUS_APPLICATION_VOIP, &error);
  if (state_ == nullptr || error != OPUS_OK) {
    last_error_ = error != OPUS_OK ? error : OPUS_INTERNAL_ERROR;
    close();
    return false;
  }

  config_ = config;

  // OPUS_APPLICATION_VOIP already biases for speech; SIGNAL_VOICE additionally
  // tells the encoder not to second-guess that on ambiguous input.
  opus_encoder_ctl(state_, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
  opus_encoder_ctl(state_, OPUS_SET_BITRATE(config.bitrate_bps));
  opus_encoder_ctl(state_, OPUS_SET_VBR(config.vbr ? 1 : 0));
  opus_encoder_ctl(state_, OPUS_SET_COMPLEXITY(config.complexity));
  opus_encoder_ctl(state_, OPUS_SET_INBAND_FEC(config.inband_fec ? 1 : 0));
  opus_encoder_ctl(state_,
                   OPUS_SET_PACKET_LOSS_PERC(config.expected_loss_percent));
  opus_encoder_ctl(state_, OPUS_SET_DTX(config.dtx ? 1 : 0));

  last_error_ = OPUS_OK;
  return true;
}

Encoder::Result Encoder::encode(std::span<const std::int16_t> pcm,
                                ByteSpan out) noexcept {
  Result result;

  if (state_ == nullptr) {
    result.error = OPUS_INVALID_STATE;
    return result;
  }

  // Exactly one frame, no padding and no truncation. Encoding a different
  // duration than the caller believes would desynchronise the wire timestamp
  // from the audio actually sent, which is close to undiagnosable later.
  const std::size_t expected = static_cast<std::size_t>(config_.frame_samples) *
                               static_cast<std::size_t>(config_.channels);
  if (pcm.size() != expected) {
    result.error = OPUS_BAD_ARG;
    return result;
  }
  if (out.empty()) {
    result.error = OPUS_BUFFER_TOO_SMALL;
    return result;
  }

  const opus_int32 written =
      opus_encode(state_, pcm.data(), static_cast<int>(config_.frame_samples),
                  reinterpret_cast<unsigned char*>(out.data()),
                  static_cast<opus_int32>(out.size()));

  if (written < 0) {
    result.error = written;  // Opus returns the error code as a negative length
    last_error_ = written;
    return result;
  }

  result.bytes = static_cast<std::size_t>(written);

  // Opus documents that DTX produces a packet of one byte or less, and that
  // such a packet need not be transmitted at all.
  result.dtx_skipped = config_.dtx && written <= 1;
  return result;
}

bool Encoder::set_bitrate(std::int32_t bitrate_bps) noexcept {
  if (state_ == nullptr) return false;
  const int rc = opus_encoder_ctl(state_, OPUS_SET_BITRATE(bitrate_bps));
  if (rc != OPUS_OK) {
    last_error_ = rc;
    return false;
  }
  config_.bitrate_bps = bitrate_bps;
  return true;
}

bool Encoder::set_expected_loss_percent(int percent) noexcept {
  if (state_ == nullptr || percent < 0 || percent > 100) return false;
  const int rc = opus_encoder_ctl(state_, OPUS_SET_PACKET_LOSS_PERC(percent));
  if (rc != OPUS_OK) {
    last_error_ = rc;
    return false;
  }
  config_.expected_loss_percent = percent;
  return true;
}

bool Encoder::set_inband_fec(bool enabled) noexcept {
  if (state_ == nullptr) return false;
  const int rc = opus_encoder_ctl(state_, OPUS_SET_INBAND_FEC(enabled ? 1 : 0));
  if (rc != OPUS_OK) {
    last_error_ = rc;
    return false;
  }
  config_.inband_fec = enabled;
  return true;
}

bool Encoder::set_complexity(int complexity) noexcept {
  if (state_ == nullptr || complexity < 0 || complexity > 10) return false;
  const int rc = opus_encoder_ctl(state_, OPUS_SET_COMPLEXITY(complexity));
  if (rc != OPUS_OK) {
    last_error_ = rc;
    return false;
  }
  config_.complexity = complexity;
  return true;
}

std::int32_t Encoder::query_bitrate_bps() const noexcept {
  if (state_ == nullptr) return 0;
  opus_int32 value = 0;
  if (opus_encoder_ctl(state_, OPUS_GET_BITRATE(&value)) != OPUS_OK) return 0;
  return value;
}

bool Encoder::query_inband_fec() const noexcept {
  if (state_ == nullptr) return false;
  opus_int32 value = 0;
  if (opus_encoder_ctl(state_, OPUS_GET_INBAND_FEC(&value)) != OPUS_OK) {
    return false;
  }
  return value != 0;
}

int Encoder::query_expected_loss_percent() const noexcept {
  if (state_ == nullptr) return 0;
  opus_int32 value = 0;
  if (opus_encoder_ctl(state_, OPUS_GET_PACKET_LOSS_PERC(&value)) != OPUS_OK) {
    return 0;
  }
  return static_cast<int>(value);
}

// ------------------------------------------------------------------ Decoder

Decoder::~Decoder() { close(); }

Decoder::Decoder(Decoder&& other) noexcept
    : state_(other.state_),
      config_(other.config_),
      last_error_(other.last_error_) {
  other.state_ = nullptr;
}

Decoder& Decoder::operator=(Decoder&& other) noexcept {
  if (this != &other) {
    close();
    state_ = other.state_;
    config_ = other.config_;
    last_error_ = other.last_error_;
    other.state_ = nullptr;
  }
  return *this;
}

void Decoder::close() noexcept {
  if (state_ != nullptr) {
    opus_decoder_destroy(state_);
    state_ = nullptr;
  }
}

const char* Decoder::to_string(Source source) noexcept {
  switch (source) {
    case Source::Packet:
      return "packet";
    case Source::Concealment:
      return "concealment";
    case Source::ForwardCorrection:
      return "forward_correction";
  }
  return "invalid";
}

bool Decoder::open(const DecoderConfig& config) noexcept {
  close();

  if (config.channels != 1 && config.channels != 2) {
    last_error_ = OPUS_BAD_ARG;
    return false;
  }
  if (!is_valid_frame_samples(config.sample_rate_hz, config.frame_samples)) {
    last_error_ = OPUS_BAD_ARG;
    return false;
  }

  int error = OPUS_OK;
  state_ = opus_decoder_create(static_cast<opus_int32>(config.sample_rate_hz),
                               config.channels, &error);
  if (state_ == nullptr || error != OPUS_OK) {
    last_error_ = error != OPUS_OK ? error : OPUS_INTERNAL_ERROR;
    close();
    return false;
  }

  config_ = config;
  last_error_ = OPUS_OK;
  return true;
}

namespace {

// The three decode paths differ only in what is passed for the packet and
// whether the FEC flag is set, so the argument checking lives in one place.
Decoder::Result run_decode(OpusDecoder* state, const DecoderConfig& config,
                           const unsigned char* data, opus_int32 length,
                           std::span<std::int16_t> pcm, int use_fec,
                           Decoder::Source source, int& last_error) {
  Decoder::Result result;
  result.source = source;

  if (state == nullptr) {
    result.error = OPUS_INVALID_STATE;
    return result;
  }

  const std::size_t needed = static_cast<std::size_t>(config.frame_samples) *
                             static_cast<std::size_t>(config.channels);
  if (pcm.size() < needed) {
    result.error = OPUS_BUFFER_TOO_SMALL;
    return result;
  }

  const int decoded =
      opus_decode(state, data, length, pcm.data(),
                  static_cast<int>(config.frame_samples), use_fec);

  if (decoded < 0) {
    result.error = decoded;
    last_error = decoded;
    return result;
  }

  result.samples = static_cast<std::size_t>(decoded) *
                   static_cast<std::size_t>(config.channels);
  return result;
}

}  // namespace

Decoder::Result Decoder::decode(ByteView packet,
                                std::span<std::int16_t> pcm) noexcept {
  if (packet.empty()) {
    Result result;
    result.error = OPUS_BAD_ARG;
    return result;
  }
  return run_decode(state_, config_,
                    reinterpret_cast<const unsigned char*>(packet.data()),
                    static_cast<opus_int32>(packet.size()), pcm, 0,
                    Source::Packet, last_error_);
}

Decoder::Result Decoder::conceal(std::span<std::int16_t> pcm) noexcept {
  // A null packet with zero length is how Opus is asked to invent a frame. It
  // extrapolates from its own decode history, which is why reset_state() exists
  // for talkspurt boundaries -- extrapolating across a two-second silence would
  // be worse than starting clean.
  return run_decode(state_, config_, nullptr, 0, pcm, 0, Source::Concealment,
                    last_error_);
}

Decoder::Result Decoder::recover_previous(
    ByteView next_packet, std::span<std::int16_t> pcm) noexcept {
  if (next_packet.empty()) {
    Result result;
    result.source = Source::ForwardCorrection;
    result.error = OPUS_BAD_ARG;
    return result;
  }
  // decode_fec = 1 asks for the frame BEFORE this packet, reconstructed from
  // the redundancy carried inside it. An error here usually means there is no
  // usable redundancy, which is a normal outcome rather than a fault: the
  // sender may have had FEC disabled, or Opus may have judged the bitrate too
  // low to afford it. Callers fall back to conceal().
  return run_decode(state_, config_,
                    reinterpret_cast<const unsigned char*>(next_packet.data()),
                    static_cast<opus_int32>(next_packet.size()), pcm, 1,
                    Source::ForwardCorrection, last_error_);
}

bool Decoder::reset_state() noexcept {
  if (state_ == nullptr) return false;
  const int rc = opus_decoder_ctl(state_, OPUS_RESET_STATE);
  if (rc != OPUS_OK) {
    last_error_ = rc;
    return false;
  }
  return true;
}

bool Decoder::packet_has_redundancy(ByteView packet) noexcept {
  if (packet.empty()) return false;
  // Returns 1 when LBRR redundancy is present, 0 when not, and a negative error
  // for a corrupt packet -- which is treated as "no redundancy" here, since the
  // caller's only decision is whether attempting recovery is worthwhile.
  return opus_packet_has_lbrr(
             reinterpret_cast<const unsigned char*>(packet.data()),
             static_cast<opus_int32>(packet.size())) == 1;
}

std::size_t Decoder::packet_samples(ByteView packet,
                                    std::uint32_t sample_rate_hz) noexcept {
  if (packet.empty()) return 0;
  const int samples = opus_packet_get_nb_samples(
      reinterpret_cast<const unsigned char*>(packet.data()),
      static_cast<opus_int32>(packet.size()),
      static_cast<opus_int32>(sample_rate_hz));
  return samples > 0 ? static_cast<std::size_t>(samples) : 0;
}

}  // namespace radio::audio
