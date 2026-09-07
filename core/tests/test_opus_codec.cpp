#include "radio/opus_codec.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "radio/proto.hpp"

using namespace radio;
using namespace radio::audio;

namespace {

constexpr std::size_t kFrame = kFrameSamples;  // 960 = 20 ms at 48 kHz
constexpr double kAmplitude = 12'000.0;

// A signal concealment cannot extrapolate well: gliding pitch, three harmonics,
// and an amplitude envelope. A steady sine would flatter concealment enormously
// and make the FEC comparison below meaningless.
void fill_speechlike(std::vector<std::int16_t>& pcm, double start_seconds) {
  for (std::size_t i = 0; i < pcm.size(); ++i) {
    const double t = start_seconds + static_cast<double>(i) / 48'000.0;
    const double f0 = 120.0 + 60.0 * std::sin(2.0 * M_PI * 1.7 * t);
    double sample = 0.55 * std::sin(2.0 * M_PI * f0 * t) +
                    0.25 * std::sin(2.0 * M_PI * 2.0 * f0 * t + 0.6) +
                    0.12 * std::sin(2.0 * M_PI * 3.0 * f0 * t + 1.1);
    sample *= 0.6 + 0.4 * std::sin(2.0 * M_PI * 3.1 * t);
    pcm[i] = static_cast<std::int16_t>(sample * kAmplitude);
  }
}

double mean_absolute_error(const std::vector<std::int16_t>& a,
                           const std::vector<std::int16_t>& b) {
  REQUIRE(a.size() == b.size());
  double total = 0.0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    total += std::abs(static_cast<double>(a[i]) - static_cast<double>(b[i]));
  }
  return total / static_cast<double>(a.size());
}

// Encodes `count` frames of continuous audio and returns the packets.
std::vector<std::vector<std::byte>> encode_stream(Encoder& encoder,
                                                  std::size_t count) {
  std::vector<std::vector<std::byte>> packets;
  std::vector<std::int16_t> frame(kFrame);
  std::vector<std::byte> buffer(proto::kMaxPayload);
  for (std::size_t f = 0; f < count; ++f) {
    fill_speechlike(frame, static_cast<double>(f) * 0.02);
    const auto result = encoder.encode(frame, buffer);
    REQUIRE(result.ok());
    packets.emplace_back(
        buffer.begin(),
        buffer.begin() + static_cast<std::ptrdiff_t>(result.bytes));
  }
  return packets;
}

}  // namespace

TEST_CASE("only frame durations Opus accepts are valid") {
  // 2.5, 5, 10, 20, 40, 60 ms at 48 kHz.
  for (const std::uint32_t samples : {120u, 240u, 480u, 960u, 1920u, 2880u}) {
    CHECK(is_valid_frame_samples(48'000, samples));
  }
  for (const std::uint32_t samples : {0u, 1u, 100u, 512u, 961u, 1000u, 4096u}) {
    CHECK_FALSE(is_valid_frame_samples(48'000, samples));
  }
  // The check is rate-relative rather than a hard-coded table. Note 960 samples
  // is valid at 16 kHz too -- it is 60 ms there rather than 20 ms -- so a table
  // keyed on the sample count alone would be wrong.
  CHECK(is_valid_frame_samples(16'000, 320));  // 20 ms at 16 kHz
  CHECK(is_valid_frame_samples(16'000, 960));  // 60 ms at 16 kHz
  CHECK_FALSE(is_valid_frame_samples(16'000, 500));
  CHECK_FALSE(is_valid_frame_samples(16'000, 480));  // 30 ms, not a valid size
}

TEST_CASE("codec handles are move-only") {
  static_assert(!std::is_copy_constructible_v<Encoder>);
  static_assert(!std::is_copy_assignable_v<Encoder>);
  static_assert(std::is_move_constructible_v<Encoder>);
  static_assert(!std::is_copy_constructible_v<Decoder>);
  static_assert(std::is_move_constructible_v<Decoder>);
  SUCCEED();
}

TEST_CASE("an unopened codec refuses work rather than crashing") {
  Encoder encoder;
  CHECK_FALSE(encoder.is_open());
  std::vector<std::int16_t> pcm(kFrame);
  std::vector<std::byte> out(1024);
  CHECK_FALSE(encoder.encode(pcm, out).ok());

  Decoder decoder;
  CHECK_FALSE(decoder.is_open());
  CHECK_FALSE(decoder.conceal(pcm).ok());
}

TEST_CASE("a default configuration opens") {
  Encoder encoder;
  REQUIRE(encoder.open(EncoderConfig{}));
  CHECK(encoder.is_open());

  Decoder decoder;
  REQUIRE(decoder.open(DecoderConfig{}));
  CHECK(decoder.is_open());
}

TEST_CASE("invalid configurations are rejected at open") {
  Encoder encoder;

  EncoderConfig bad_frame;
  bad_frame.frame_samples = 1000;  // not a duration Opus accepts
  CHECK_FALSE(encoder.open(bad_frame));

  EncoderConfig bad_channels;
  bad_channels.channels = 3;
  CHECK_FALSE(encoder.open(bad_channels));

  EncoderConfig bad_complexity;
  bad_complexity.complexity = 11;
  CHECK_FALSE(encoder.open(bad_complexity));

  EncoderConfig bad_loss;
  bad_loss.expected_loss_percent = 101;
  CHECK_FALSE(encoder.open(bad_loss));
}

TEST_CASE("FEC without a loss estimate is rejected, not silently ignored") {
  // Measured: at 32 kbps with expected_loss_percent = 0, Opus emits redundancy
  // in 0 of 30 packets. Accepting this configuration would produce something
  // that reports FEC enabled and protects nothing, which is the usual route to
  // concluding that Opus FEC does not work.
  EncoderConfig config;
  config.inband_fec = true;
  config.expected_loss_percent = 0;

  Encoder encoder;
  CHECK_FALSE(encoder.open(config));

  config.expected_loss_percent = 10;
  CHECK(encoder.open(config));
}

TEST_CASE("encoding requires exactly one frame of samples") {
  // Padding or truncating would desynchronise the wire timestamp from the audio
  // actually sent, which is close to undiagnosable after the fact.
  Encoder encoder;
  REQUIRE(encoder.open(EncoderConfig{}));
  std::vector<std::byte> out(1024);

  std::vector<std::int16_t> too_short(kFrame - 1);
  CHECK_FALSE(encoder.encode(too_short, out).ok());

  std::vector<std::int16_t> too_long(kFrame + 1);
  CHECK_FALSE(encoder.encode(too_long, out).ok());

  std::vector<std::int16_t> exact(kFrame);
  CHECK(encoder.encode(exact, out).ok());
}

TEST_CASE("a 20 ms frame at 32 kbps lands near the expected size") {
  Encoder encoder;
  REQUIRE(encoder.open(EncoderConfig{}));

  const auto packets = encode_stream(encoder, 30);
  std::size_t total = 0;
  for (const auto& packet : packets) total += packet.size();
  const double average =
      static_cast<double>(total) / static_cast<double>(packets.size());

  // 32 kbps over 20 ms is 80 bytes. VBR moves it around, so this is a sanity
  // band rather than an exact figure.
  CHECK(average > 40.0);
  CHECK(average < 160.0);
}

TEST_CASE("encode then decode returns a full frame of samples") {
  Encoder encoder;
  Decoder decoder;
  REQUIRE(encoder.open(EncoderConfig{}));
  REQUIRE(decoder.open(DecoderConfig{}));

  std::vector<std::int16_t> input(kFrame);
  fill_speechlike(input, 0.0);
  std::vector<std::byte> packet(1024);
  const auto encoded = encoder.encode(input, packet);
  REQUIRE(encoded.ok());

  std::vector<std::int16_t> output(kFrame);
  const auto decoded =
      decoder.decode(ByteView{packet.data(), encoded.bytes}, output);
  REQUIRE(decoded.ok());
  CHECK(decoded.samples == kFrame);
  CHECK(decoded.source == Decoder::Source::Packet);
}

TEST_CASE("a packet reports how many samples it carries without decoding") {
  Encoder encoder;
  REQUIRE(encoder.open(EncoderConfig{}));
  std::vector<std::int16_t> input(kFrame);
  fill_speechlike(input, 0.0);
  std::vector<std::byte> packet(1024);
  const auto encoded = encoder.encode(input, packet);
  REQUIRE(encoded.ok());

  CHECK(Decoder::packet_samples(ByteView{packet.data(), encoded.bytes},
                                48'000) == kFrame);
  CHECK(Decoder::packet_samples(ByteView{}, 48'000) == 0);
}

TEST_CASE("a decode buffer smaller than a frame is refused") {
  Encoder encoder;
  Decoder decoder;
  REQUIRE(encoder.open(EncoderConfig{}));
  REQUIRE(decoder.open(DecoderConfig{}));

  std::vector<std::int16_t> input(kFrame);
  std::vector<std::byte> packet(1024);
  const auto encoded = encoder.encode(input, packet);
  REQUIRE(encoded.ok());

  std::vector<std::int16_t> too_small(kFrame - 1);
  CHECK_FALSE(
      decoder.decode(ByteView{packet.data(), encoded.bytes}, too_small).ok());
}

TEST_CASE("DTX emits a one-byte packet for silence") {
  EncoderConfig config;
  config.dtx = true;
  Encoder encoder;
  REQUIRE(encoder.open(config));

  std::vector<std::int16_t> silence(kFrame, 0);
  std::vector<std::byte> out(1024);

  // Opus needs a few frames to decide the input really is silent.
  bool saw_skip = false;
  for (int f = 0; f < 20; ++f) {
    const auto result = encoder.encode(silence, out);
    REQUIRE(result.ok());
    if (result.dtx_skipped) {
      saw_skip = true;
      CHECK(result.bytes <= 1);
    }
  }
  CHECK(saw_skip);
}

TEST_CASE("settings are read back from the library, not from our request") {
  // Opus clamps and reinterprets several settings, so what it is actually doing
  // is not always what was asked for.
  EncoderConfig config;
  config.bitrate_bps = 24'000;
  config.inband_fec = true;
  config.expected_loss_percent = 15;

  Encoder encoder;
  REQUIRE(encoder.open(config));
  CHECK(encoder.query_bitrate_bps() == 24'000);
  CHECK(encoder.query_inband_fec());
  CHECK(encoder.query_expected_loss_percent() == 15);

  REQUIRE(encoder.set_bitrate(40'000));
  CHECK(encoder.query_bitrate_bps() == 40'000);

  REQUIRE(encoder.set_expected_loss_percent(30));
  CHECK(encoder.query_expected_loss_percent() == 30);

  REQUIRE(encoder.set_inband_fec(false));
  CHECK_FALSE(encoder.query_inband_fec());

  CHECK_FALSE(encoder.set_expected_loss_percent(-1));
  CHECK_FALSE(encoder.set_complexity(99));
}

TEST_CASE("moving a codec transfers the state and disarms the source") {
  Encoder encoder;
  REQUIRE(encoder.open(EncoderConfig{}));

  Encoder moved = std::move(encoder);
  CHECK_FALSE(encoder.is_open());
  CHECK(moved.is_open());

  // Still usable, which proves the moved-from destructor did not free it.
  std::vector<std::int16_t> pcm(kFrame);
  std::vector<std::byte> out(1024);
  CHECK(moved.encode(pcm, out).ok());
}

TEST_CASE("redundancy is present only when the encoder can afford it") {
  // The measured preconditions, asserted so a future change to the defaults
  // cannot quietly disable FEC.
  std::vector<std::int16_t> frame(kFrame);
  std::vector<std::byte> out(1024);

  const auto count_with_redundancy = [&](std::int32_t bitrate, int loss) {
    EncoderConfig config;
    config.bitrate_bps = bitrate;
    config.inband_fec = loss > 0;
    config.expected_loss_percent = loss;
    Encoder encoder;
    REQUIRE(encoder.open(config));

    int found = 0;
    for (int f = 0; f < 30; ++f) {
      fill_speechlike(frame, static_cast<double>(f) * 0.02);
      const auto result = encoder.encode(frame, out);
      REQUIRE(result.ok());
      if (Decoder::packet_has_redundancy(ByteView{out.data(), result.bytes})) {
        ++found;
      }
    }
    return found;
  };

  CHECK(count_with_redundancy(32'000, 0) == 0);   // no loss estimate, no FEC
  CHECK(count_with_redundancy(32'000, 20) > 15);  // measured 25 of 30
  CHECK(count_with_redundancy(16'000, 20) == 0);  // rate too low to afford it
}

TEST_CASE("FEC recovery is far closer to the truth than concealment") {
  // The claim the whole loss-recovery design rests on, measured.
  //
  // The reference is the frame that WOULD have been decoded had nothing been
  // lost -- not the original PCM. Comparing against the original measures the
  // codec's own lossiness, which is far larger than the difference being tested
  // and completely swamps it. Choosing the reference that isolates the variable
  // is the whole trick here.
  constexpr std::size_t kFrames = 30;
  constexpr std::size_t kLost = 20;

  EncoderConfig config;
  config.inband_fec = true;
  config.expected_loss_percent = 20;
  Encoder encoder;
  REQUIRE(encoder.open(config));

  const auto packets = encode_stream(encoder, kFrames);
  REQUIRE(Decoder::packet_has_redundancy(
      ByteView{packets[kLost + 1].data(), packets[kLost + 1].size()}));

  DecoderConfig decoder_config;
  std::vector<std::int16_t> scratch(kFrame);
  std::vector<std::int16_t> reference(kFrame);
  std::vector<std::int16_t> concealed(kFrame);
  std::vector<std::int16_t> recovered(kFrame);

  const auto feed = [&](Decoder& decoder, std::size_t upto) {
    for (std::size_t f = 0; f < upto; ++f) {
      REQUIRE(
          decoder
              .decode(ByteView{packets[f].data(), packets[f].size()}, scratch)
              .ok());
    }
  };

  // What the listener would have heard if the packet had arrived.
  {
    Decoder decoder;
    REQUIRE(decoder.open(decoder_config));
    feed(decoder, kLost);
    REQUIRE(decoder
                .decode(ByteView{packets[kLost].data(), packets[kLost].size()},
                        reference)
                .ok());
  }

  // Concealed: nothing available, the codec extrapolates.
  {
    Decoder decoder;
    REQUIRE(decoder.open(decoder_config));
    feed(decoder, kLost);
    const auto result = decoder.conceal(concealed);
    REQUIRE(result.ok());
    CHECK(result.source == Decoder::Source::Concealment);
  }

  // Recovered from the redundancy inside the NEXT packet.
  {
    Decoder decoder;
    REQUIRE(decoder.open(decoder_config));
    feed(decoder, kLost);
    const auto result = decoder.recover_previous(
        ByteView{packets[kLost + 1].data(), packets[kLost + 1].size()},
        recovered);
    REQUIRE(result.ok());
    CHECK(result.source == Decoder::Source::ForwardCorrection);
  }

  const double conceal_error = mean_absolute_error(reference, concealed);
  const double fec_error = mean_absolute_error(reference, recovered);

  // Measured on this signal: concealment ~2520, FEC ~27, roughly 90x closer.
  // Asserting 10x leaves an order of magnitude of headroom, so this is a claim
  // about the mechanism rather than about one build of Opus.
  CHECK(conceal_error > 200.0);             // concealment really is far off
  CHECK(fec_error * 10.0 < conceal_error);  // and FEC really does recover it
}

TEST_CASE("without redundancy, recovery succeeds but conceals instead") {
  // The trap this API exists to expose. Opus does not report failure when there
  // is nothing to recover from -- it silently conceals and returns success. A
  // metric that counted every successful call as an FEC recovery would be
  // counting concealed frames.
  constexpr std::size_t kFrames = 30;
  constexpr std::size_t kLost = 20;

  EncoderConfig config;
  config.inband_fec = false;  // no redundancy at all
  Encoder encoder;
  REQUIRE(encoder.open(config));
  const auto packets = encode_stream(encoder, kFrames);

  CHECK_FALSE(Decoder::packet_has_redundancy(
      ByteView{packets[kLost + 1].data(), packets[kLost + 1].size()}));

  DecoderConfig decoder_config;
  std::vector<std::int16_t> scratch(kFrame);
  std::vector<std::int16_t> concealed(kFrame);
  std::vector<std::int16_t> pseudo_recovered(kFrame);

  const auto feed = [&](Decoder& decoder) {
    for (std::size_t f = 0; f < kLost; ++f) {
      REQUIRE(
          decoder
              .decode(ByteView{packets[f].data(), packets[f].size()}, scratch)
              .ok());
    }
  };

  {
    Decoder decoder;
    REQUIRE(decoder.open(decoder_config));
    feed(decoder);
    REQUIRE(decoder.conceal(concealed).ok());
  }
  {
    Decoder decoder;
    REQUIRE(decoder.open(decoder_config));
    feed(decoder);
    // Reports success despite there being no redundancy to use.
    const auto result = decoder.recover_previous(
        ByteView{packets[kLost + 1].data(), packets[kLost + 1].size()},
        pseudo_recovered);
    CHECK(result.ok());
  }

  // Identical output, because both paths concealed. This is why
  // packet_has_redundancy() has to be consulted before crediting FEC.
  CHECK(mean_absolute_error(concealed, pseudo_recovered) < 1.0);
}

TEST_CASE("resetting the decoder clears its extrapolation history") {
  // Used at a talkspurt boundary: concealing from audio that ended two seconds
  // ago is worse than starting clean.
  Encoder encoder;
  Decoder decoder;
  REQUIRE(encoder.open(EncoderConfig{}));
  REQUIRE(decoder.open(DecoderConfig{}));

  const auto packets = encode_stream(encoder, 10);
  std::vector<std::int16_t> scratch(kFrame);
  for (const auto& packet : packets) {
    REQUIRE(
        decoder.decode(ByteView{packet.data(), packet.size()}, scratch).ok());
  }

  std::vector<std::int16_t> before_reset(kFrame);
  REQUIRE(decoder.conceal(before_reset).ok());

  REQUIRE(decoder.reset_state());
  std::vector<std::int16_t> after_reset(kFrame);
  REQUIRE(decoder.conceal(after_reset).ok());

  // With no history, concealment has nothing to extrapolate from and produces
  // near-silence instead of a continuation of the previous audio.
  double energy_before = 0.0;
  double energy_after = 0.0;
  for (std::size_t i = 0; i < kFrame; ++i) {
    energy_before += std::abs(static_cast<double>(before_reset[i]));
    energy_after += std::abs(static_cast<double>(after_reset[i]));
  }
  CHECK(energy_before > energy_after);
}

TEST_CASE("a corrupt packet is rejected rather than decoded") {
  Decoder decoder;
  REQUIRE(decoder.open(DecoderConfig{}));
  std::vector<std::int16_t> pcm(kFrame);

  const std::vector<std::byte> garbage(40, static_cast<std::byte>(0xFF));
  const auto result = decoder.decode(garbage, pcm);
  CHECK_FALSE(result.ok());

  CHECK_FALSE(decoder.decode(ByteView{}, pcm).ok());
  CHECK_FALSE(decoder.recover_previous(ByteView{}, pcm).ok());
}

TEST_CASE("opus error codes render as text") {
  CHECK(error_string(0) != nullptr);
  CHECK(std::string_view{error_string(0)}.size() > 0);
}
