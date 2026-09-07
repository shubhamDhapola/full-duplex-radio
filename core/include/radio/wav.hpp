// Reading and writing 16-bit PCM WAV files.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace radio::audio {

// NOT for the audio path. This allocates and performs file I/O, both of which
// are forbidden anywhere near a real-time callback. It exists so fixtures can
// be read in and results written out: the benchmark harness uses it, and it is
// what makes the pipeline's behaviour audible rather than merely tabulated.

struct WavInfo {
  std::uint32_t sample_rate_hz = 0;
  std::uint16_t channels = 0;
  std::uint16_t bits_per_sample = 0;
  // Samples per channel. `frames * channels` is the length of the sample
  // vector.
  std::uint64_t frames = 0;

  [[nodiscard]] double duration_seconds() const noexcept;
};

enum class WavError : std::uint8_t {
  None = 0,
  CannotOpen,
  TooShort,
  NotRiff,
  NotWave,
  MissingFmtChunk,
  MissingDataChunk,
  MalformedChunk,
  // Compressed or float WAV. Deliberately not supported: decoding those is a
  // separate problem, and the fix is one ffmpeg command away.
  UnsupportedEncoding,
  UnsupportedBitDepth,
  UnsupportedChannelCount,
  TruncatedData,
  WriteFailed,
};

[[nodiscard]] const char* to_string(WavError error) noexcept;

// A hint the caller can print. Being told "unsupported bit depth" is much less
// useful than being told what to run.
[[nodiscard]] const char* conversion_hint(WavError error) noexcept;

struct WavReadResult {
  WavError error = WavError::None;
  WavInfo info{};
  // Interleaved, so a stereo file is L R L R. Use downmix_to_mono for a single
  // channel.
  std::vector<std::int16_t> samples;

  [[nodiscard]] bool ok() const noexcept { return error == WavError::None; }
};

// Reads a 16-bit PCM WAV.
//
// Handles the awkward parts of real files rather than only the canonical
// 44-byte header: unknown chunks are skipped, odd-sized chunks are padded to
// even as the specification requires, and WAVE_FORMAT_EXTENSIBLE is accepted
// when its subformat is PCM. Files produced by common tools hit at least one of
// those, so rejecting them would make the reader useless in practice.
//
// The sample rate is reported rather than enforced. Resampling is out of scope,
// so it is the caller's job to decide whether the rate is acceptable -- which
// keeps pipeline policy out of a file parser.
[[nodiscard]] WavReadResult read_wav(const std::string& path);

// Averages interleaved channels down to one.
//
// Averaging rather than taking the left channel: a recording with the voice
// panned right, or with one dead channel, would otherwise come out silent.
[[nodiscard]] std::vector<std::int16_t> downmix_to_mono(
    std::span<const std::int16_t> interleaved, std::uint16_t channels);

// Writes 16-bit PCM. `interleaved` must be a whole number of frames.
[[nodiscard]] WavError write_wav(const std::string& path,
                                 std::span<const std::int16_t> interleaved,
                                 std::uint32_t sample_rate_hz,
                                 std::uint16_t channels);

}  // namespace radio::audio
