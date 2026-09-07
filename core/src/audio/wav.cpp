#include "radio/wav.hpp"

#include <array>
#include <cstdio>
#include <cstring>

#include "radio/bytes.hpp"

namespace radio::audio {
namespace {

constexpr std::uint16_t kFormatPcm = 0x0001;
// WAVE_FORMAT_EXTENSIBLE. Emitted by a lot of Windows tooling and by some
// converters even for plain 16-bit PCM, so refusing it would reject files that
// are perfectly ordinary.
constexpr std::uint16_t kFormatExtensible = 0xFFFE;

constexpr std::size_t kRiffHeaderBytes = 12;
constexpr std::size_t kChunkHeaderBytes = 8;
constexpr std::size_t kMinFmtBytes = 16;

bool tag_equals(const std::byte* p, const char (&tag)[5]) {
  for (int i = 0; i < 4; ++i) {
    if (std::to_integer<std::uint8_t>(p[i]) !=
        static_cast<std::uint8_t>(tag[i])) {
      return false;
    }
  }
  return true;
}

std::vector<std::byte> read_whole_file(const std::string& path, bool& opened) {
  std::vector<std::byte> data;
  opened = false;

  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) return data;
  opened = true;

  std::fseek(file, 0, SEEK_END);
  const long size = std::ftell(file);
  std::fseek(file, 0, SEEK_SET);

  if (size > 0) {
    data.resize(static_cast<std::size_t>(size));
    const std::size_t read = std::fread(data.data(), 1, data.size(), file);
    data.resize(read);
  }
  std::fclose(file);
  return data;
}

}  // namespace

double WavInfo::duration_seconds() const noexcept {
  if (sample_rate_hz == 0) return 0.0;
  return static_cast<double>(frames) / static_cast<double>(sample_rate_hz);
}

const char* to_string(WavError error) noexcept {
  switch (error) {
    case WavError::None:
      return "none";
    case WavError::CannotOpen:
      return "cannot open file";
    case WavError::TooShort:
      return "file too short to be a WAV";
    case WavError::NotRiff:
      return "not a RIFF file";
    case WavError::NotWave:
      return "RIFF file is not WAVE";
    case WavError::MissingFmtChunk:
      return "no fmt chunk";
    case WavError::MissingDataChunk:
      return "no data chunk";
    case WavError::MalformedChunk:
      return "malformed chunk header or size";
    case WavError::UnsupportedEncoding:
      return "not uncompressed PCM";
    case WavError::UnsupportedBitDepth:
      return "not 16-bit";
    case WavError::UnsupportedChannelCount:
      return "unsupported channel count";
    case WavError::TruncatedData:
      return "data chunk shorter than its declared size";
    case WavError::WriteFailed:
      return "write failed";
  }
  return "invalid";
}

const char* conversion_hint(WavError error) noexcept {
  switch (error) {
    case WavError::UnsupportedEncoding:
    case WavError::UnsupportedBitDepth:
    case WavError::UnsupportedChannelCount:
      // Being told what to run is considerably more useful than being told the
      // file is unsupported.
      return "convert it first:  ffmpeg -i in.wav -ar 48000 -ac 1 "
             "-c:a pcm_s16le out.wav";
    case WavError::NotRiff:
    case WavError::NotWave:
      return "the file is not a WAV; ffmpeg can convert most formats";
    default:
      return "";
  }
}

WavReadResult read_wav(const std::string& path) {
  WavReadResult result;

  bool opened = false;
  const std::vector<std::byte> file = read_whole_file(path, opened);
  if (!opened) {
    result.error = WavError::CannotOpen;
    return result;
  }
  if (file.size() < kRiffHeaderBytes) {
    result.error = WavError::TooShort;
    return result;
  }
  if (!tag_equals(file.data(), "RIFF")) {
    result.error = WavError::NotRiff;
    return result;
  }
  if (!tag_equals(file.data() + 8, "WAVE")) {
    result.error = WavError::NotWave;
    return result;
  }

  bool have_fmt = false;
  std::uint16_t encoding = 0;
  const std::byte* data_start = nullptr;
  std::size_t data_bytes = 0;

  // Walk the chunk list rather than assuming the canonical 44-byte layout.
  // Real files interleave LIST, fact, id3 and others, and a reader that assumes
  // fmt-then-data works on files produced by exactly one tool.
  std::size_t offset = kRiffHeaderBytes;
  while (offset + kChunkHeaderBytes <= file.size()) {
    const std::byte* header = file.data() + offset;
    const std::uint32_t chunk_size = load_le32(header + 4);
    const std::size_t body = offset + kChunkHeaderBytes;

    // A declared size that runs past the end of the file is malformed. Checked
    // before it is used to index anything.
    if (chunk_size > file.size() - body) {
      // A truncated data chunk is common enough (an interrupted recording) to
      // be worth distinguishing from a corrupt header.
      if (tag_equals(header, "data")) {
        result.error = WavError::TruncatedData;
      } else {
        result.error = WavError::MalformedChunk;
      }
      return result;
    }

    if (tag_equals(header, "fmt ")) {
      if (chunk_size < kMinFmtBytes) {
        result.error = WavError::MalformedChunk;
        return result;
      }
      const std::byte* fmt = file.data() + body;
      encoding = load_le16(fmt);
      result.info.channels = load_le16(fmt + 2);
      result.info.sample_rate_hz = load_le32(fmt + 4);
      result.info.bits_per_sample = load_le16(fmt + 14);

      if (encoding == kFormatExtensible) {
        // The real encoding lives in the subformat GUID, whose first two bytes
        // hold the format tag. Anything else here is compressed.
        if (chunk_size < 40) {
          result.error = WavError::MalformedChunk;
          return result;
        }
        encoding = load_le16(fmt + 24);
      }
      have_fmt = true;
    } else if (tag_equals(header, "data")) {
      data_start = file.data() + body;
      data_bytes = chunk_size;
    }

    // Chunks are padded to an even length, and the pad byte is not counted in
    // the declared size. Missing this desynchronises the walk on any file with
    // an odd-sized chunk, which then reads a chunk id out of chunk data.
    offset = body + chunk_size + (chunk_size % 2);
  }

  if (!have_fmt) {
    result.error = WavError::MissingFmtChunk;
    return result;
  }
  if (data_start == nullptr) {
    result.error = WavError::MissingDataChunk;
    return result;
  }
  if (encoding != kFormatPcm) {
    result.error = WavError::UnsupportedEncoding;
    return result;
  }
  if (result.info.bits_per_sample != 16) {
    result.error = WavError::UnsupportedBitDepth;
    return result;
  }
  if (result.info.channels == 0 || result.info.channels > 8) {
    result.error = WavError::UnsupportedChannelCount;
    return result;
  }

  const std::size_t bytes_per_frame =
      static_cast<std::size_t>(result.info.channels) * sizeof(std::int16_t);
  const std::size_t whole = (data_bytes / bytes_per_frame) * bytes_per_frame;
  result.info.frames = whole / bytes_per_frame;

  result.samples.resize(whole / sizeof(std::int16_t));
  for (std::size_t i = 0; i < result.samples.size(); ++i) {
    // Read through the little-endian helper rather than memcpy: the file's byte
    // order is a property of the format, not of this machine.
    result.samples[i] =
        static_cast<std::int16_t>(load_le16(data_start + i * 2));
  }
  return result;
}

std::vector<std::int16_t> downmix_to_mono(
    std::span<const std::int16_t> interleaved, std::uint16_t channels) {
  std::vector<std::int16_t> mono;
  if (channels == 0) return mono;
  if (channels == 1) {
    mono.assign(interleaved.begin(), interleaved.end());
    return mono;
  }

  const std::size_t frames = interleaved.size() / channels;
  mono.resize(frames);
  for (std::size_t f = 0; f < frames; ++f) {
    // Accumulate in a wider type: summing eight int16 channels overflows.
    std::int32_t total = 0;
    for (std::uint16_t c = 0; c < channels; ++c) {
      total += interleaved[f * channels + c];
    }
    mono[f] = static_cast<std::int16_t>(total / channels);
  }
  return mono;
}

WavError write_wav(const std::string& path,
                   std::span<const std::int16_t> interleaved,
                   std::uint32_t sample_rate_hz, std::uint16_t channels) {
  if (channels == 0 || channels > 8) {
    return WavError::UnsupportedChannelCount;
  }
  if (sample_rate_hz == 0) return WavError::WriteFailed;
  if (interleaved.size() % channels != 0) return WavError::WriteFailed;

  const std::uint32_t data_bytes =
      static_cast<std::uint32_t>(interleaved.size() * sizeof(std::int16_t));
  const std::uint16_t block_align =
      static_cast<std::uint16_t>(channels * sizeof(std::int16_t));

  // The canonical 44-byte header: RIFF, then a 16-byte fmt chunk, then data.
  std::array<std::byte, 44> header{};
  std::byte* p = header.data();

  std::memcpy(p, "RIFF", 4);
  store_le32(p + 4, 36u + data_bytes);  // everything after this field
  std::memcpy(p + 8, "WAVE", 4);

  std::memcpy(p + 12, "fmt ", 4);
  store_le32(p + 16, 16u);  // fmt chunk body size
  store_le16(p + 20, kFormatPcm);
  store_le16(p + 22, channels);
  store_le32(p + 24, sample_rate_hz);
  store_le32(p + 28, sample_rate_hz * block_align);  // byte rate
  store_le16(p + 32, block_align);
  store_le16(p + 34, 16u);  // bits per sample

  std::memcpy(p + 36, "data", 4);
  store_le32(p + 40, data_bytes);

  std::FILE* file = std::fopen(path.c_str(), "wb");
  if (file == nullptr) return WavError::CannotOpen;

  bool ok = std::fwrite(header.data(), 1, header.size(), file) == header.size();

  // Written a sample at a time through the little-endian helper. A single
  // fwrite of the vector would be faster and would silently emit big-endian
  // data on a big-endian host -- the same class of bug bytes.hpp exists to
  // prevent, and no faster in any way that matters for a tool.
  std::array<std::byte, 2> encoded{};
  for (std::size_t i = 0; ok && i < interleaved.size(); ++i) {
    store_le16(encoded.data(), static_cast<std::uint16_t>(interleaved[i]));
    ok = std::fwrite(encoded.data(), 1, encoded.size(), file) == encoded.size();
  }

  if (std::fclose(file) != 0) ok = false;
  return ok ? WavError::None : WavError::WriteFailed;
}

}  // namespace radio::audio
