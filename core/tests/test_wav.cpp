#include "radio/wav.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <numeric>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "radio/bytes.hpp"

using namespace radio;
using namespace radio::audio;

namespace {

// Builds WAV bytes in memory, so the parser can be pointed at deliberately
// awkward files without shipping binary fixtures.
class WavBuilder {
 public:
  void tag(const char* four) {
    for (int i = 0; i < 4; ++i) {
      bytes_.push_back(static_cast<std::byte>(four[i]));
    }
  }
  void u16(std::uint16_t value) {
    std::array<std::byte, 2> encoded{};
    store_le16(encoded.data(), value);
    bytes_.insert(bytes_.end(), encoded.begin(), encoded.end());
  }
  void u32(std::uint32_t value) {
    std::array<std::byte, 4> encoded{};
    store_le32(encoded.data(), value);
    bytes_.insert(bytes_.end(), encoded.begin(), encoded.end());
  }
  void raw(std::span<const std::byte> body) {
    bytes_.insert(bytes_.end(), body.begin(), body.end());
  }

  // Writes a complete chunk, including the pad byte an odd-sized body requires.
  void chunk(const char* id, std::span<const std::byte> body,
             std::uint32_t declared_size_override = 0) {
    tag(id);
    u32(declared_size_override != 0 ? declared_size_override
                                    : static_cast<std::uint32_t>(body.size()));
    raw(body);
    if (body.size() % 2 != 0) bytes_.push_back(std::byte{0});
  }

  [[nodiscard]] std::vector<std::byte> finish() {
    std::vector<std::byte> out;
    out.reserve(bytes_.size() + 12);
    for (int i = 0; i < 4; ++i)
      out.push_back(static_cast<std::byte>("RIFF"[i]));
    std::array<std::byte, 4> size{};
    store_le32(size.data(), static_cast<std::uint32_t>(bytes_.size() + 4));
    out.insert(out.end(), size.begin(), size.end());
    for (int i = 0; i < 4; ++i)
      out.push_back(static_cast<std::byte>("WAVE"[i]));
    out.insert(out.end(), bytes_.begin(), bytes_.end());
    return out;
  }

 private:
  std::vector<std::byte> bytes_;
};

std::vector<std::byte> fmt_pcm(std::uint16_t channels, std::uint32_t rate,
                               std::uint16_t bits,
                               std::uint16_t encoding = 0x0001) {
  WavBuilder body;
  body.u16(encoding);
  body.u16(channels);
  body.u32(rate);
  body.u32(rate * channels * (bits / 8u));
  body.u16(static_cast<std::uint16_t>(channels * (bits / 8u)));
  body.u16(bits);
  // finish() would wrap this in RIFF, so reach for the accumulated bytes only.
  auto wrapped = body.finish();
  return {wrapped.begin() + 12, wrapped.end()};
}

std::vector<std::byte> samples_to_bytes(const std::vector<std::int16_t>& s) {
  std::vector<std::byte> out(s.size() * 2);
  for (std::size_t i = 0; i < s.size(); ++i) {
    store_le16(out.data() + i * 2, static_cast<std::uint16_t>(s[i]));
  }
  return out;
}

// A unique temporary path per test, cleaned up by the destructor.
class TempFile {
 public:
  explicit TempFile(const char* name) {
    path_ = (std::filesystem::temp_directory_path() /
             ("radio_wav_test_" + std::string(name)))
                .string();
  }
  ~TempFile() { std::remove(path_.c_str()); }
  TempFile(const TempFile&) = delete;
  TempFile& operator=(const TempFile&) = delete;

  [[nodiscard]] const std::string& path() const { return path_; }

  void write(std::span<const std::byte> bytes) const {
    std::FILE* file = std::fopen(path_.c_str(), "wb");
    REQUIRE(file != nullptr);
    if (!bytes.empty()) {
      REQUIRE(std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size());
    }
    std::fclose(file);
  }

 private:
  std::string path_;
};

}  // namespace

TEST_CASE("little-endian helpers are the mirror of the big-endian ones") {
  std::array<std::byte, 4> buffer{};
  store_le32(buffer.data(), 0x01020304u);
  CHECK(buffer[0] == std::byte{0x04});  // least significant first
  CHECK(buffer[3] == std::byte{0x01});
  CHECK(load_le32(buffer.data()) == 0x01020304u);
  CHECK(load_be32(buffer.data()) == 0x04030201u);  // the same bytes, reversed

  store_le16(buffer.data(), 0xBEEFu);
  CHECK(buffer[0] == std::byte{0xEF});
  CHECK(load_le16(buffer.data()) == 0xBEEFu);

  // Correct at every misalignment, same as the big-endian versions.
  std::array<std::byte, 12> wide{};
  for (std::size_t offset = 0; offset < 8; ++offset) {
    store_le32(wide.data() + offset, 0xDEADBEEFu);
    CHECK(load_le32(wide.data() + offset) == 0xDEADBEEFu);
  }
}

TEST_CASE("a written file reads back identically") {
  TempFile file("roundtrip.wav");
  std::vector<std::int16_t> original(4800);
  for (std::size_t i = 0; i < original.size(); ++i) {
    original[i] = static_cast<std::int16_t>((i * 37) % 30000 - 15000);
  }

  REQUIRE(write_wav(file.path(), original, 48'000, 1) == WavError::None);

  const auto read = read_wav(file.path());
  REQUIRE(read.ok());
  CHECK(read.info.sample_rate_hz == 48'000);
  CHECK(read.info.channels == 1);
  CHECK(read.info.bits_per_sample == 16);
  CHECK(read.info.frames == original.size());
  CHECK(read.samples == original);
  CHECK(read.info.duration_seconds() > 0.09);
  CHECK(read.info.duration_seconds() < 0.11);
}

TEST_CASE("extreme sample values survive the round trip") {
  // int16 min is the value a naive sign conversion mangles.
  TempFile file("extremes.wav");
  const std::vector<std::int16_t> original{0,      1,     -1,    32767,
                                           -32768, 12345, -12345};
  REQUIRE(write_wav(file.path(), original, 48'000, 1) == WavError::None);
  const auto read = read_wav(file.path());
  REQUIRE(read.ok());
  CHECK(read.samples == original);
}

TEST_CASE("stereo round-trips and interleaving is preserved") {
  TempFile file("stereo.wav");
  const std::vector<std::int16_t> interleaved{100, -100, 200, -200, 300, -300};
  REQUIRE(write_wav(file.path(), interleaved, 44'100, 2) == WavError::None);

  const auto read = read_wav(file.path());
  REQUIRE(read.ok());
  CHECK(read.info.channels == 2);
  CHECK(read.info.sample_rate_hz == 44'100);
  CHECK(read.info.frames == 3);
  CHECK(read.samples == interleaved);
}

TEST_CASE("unknown chunks are skipped rather than derailing the walk") {
  // Real files carry LIST, fact, id3 and others, often before fmt. A reader
  // that assumes fmt-then-data works only on files from one tool.
  const std::vector<std::int16_t> samples{10, 20, 30, 40};
  const auto payload = samples_to_bytes(samples);
  const std::vector<std::byte> junk(16, std::byte{0x5A});

  WavBuilder builder;
  builder.chunk("LIST", junk);
  const auto fmt = fmt_pcm(1, 48'000, 16);
  builder.chunk("fmt ", fmt);
  builder.chunk("fact", junk);
  builder.chunk("data", payload);
  builder.chunk("id3 ", junk);

  TempFile file("chunks.wav");
  file.write(builder.finish());

  const auto read = read_wav(file.path());
  REQUIRE(read.ok());
  CHECK(read.samples == samples);
}

TEST_CASE("an odd-sized chunk's pad byte is accounted for") {
  // Chunks are padded to an even length and the pad byte is not counted in the
  // declared size. Missing this desynchronises the walk, and the next "chunk
  // id" it reads is actually chunk data.
  const std::vector<std::int16_t> samples{1, 2, 3, 4, 5, 6};
  const auto payload = samples_to_bytes(samples);
  const std::vector<std::byte> odd(7, std::byte{0x11});  // odd length

  WavBuilder builder;
  const auto fmt = fmt_pcm(1, 48'000, 16);
  builder.chunk("fmt ", fmt);
  builder.chunk("odd ", odd);
  builder.chunk("data", payload);

  TempFile file("odd.wav");
  file.write(builder.finish());

  const auto read = read_wav(file.path());
  REQUIRE(read.ok());
  CHECK(read.samples == samples);
}

TEST_CASE("WAVE_FORMAT_EXTENSIBLE carrying PCM is accepted") {
  // Emitted by a lot of Windows tooling even for plain 16-bit PCM, so rejecting
  // it would refuse perfectly ordinary files. The real encoding is in the
  // subformat GUID rather than the format tag.
  WavBuilder fmt;
  fmt.u16(0xFFFE);  // extensible
  fmt.u16(1);
  fmt.u32(48'000);
  fmt.u32(96'000);
  fmt.u16(2);
  fmt.u16(16);
  fmt.u16(22);      // cbSize
  fmt.u16(16);      // valid bits
  fmt.u32(0x4);     // channel mask
  fmt.u16(0x0001);  // subformat: PCM
  fmt.u16(0);
  fmt.u32(0x00100000);
  fmt.u32(0xAA000080);
  fmt.u32(0x719B3800);
  const auto wrapped = fmt.finish();
  const std::vector<std::byte> fmt_body(wrapped.begin() + 12, wrapped.end());

  const std::vector<std::int16_t> samples{7, 8, 9, 10};
  WavBuilder builder;
  builder.chunk("fmt ", fmt_body);
  builder.chunk("data", samples_to_bytes(samples));

  TempFile file("extensible.wav");
  file.write(builder.finish());

  const auto read = read_wav(file.path());
  REQUIRE(read.ok());
  CHECK(read.info.channels == 1);
  CHECK(read.samples == samples);
}

TEST_CASE("compressed and float encodings are rejected with a usable hint") {
  const std::vector<std::int16_t> samples{1, 2};

  const auto try_encoding = [&](std::uint16_t encoding, std::uint16_t bits) {
    WavBuilder builder;
    const auto fmt = fmt_pcm(1, 48'000, bits, encoding);
    builder.chunk("fmt ", fmt);
    builder.chunk("data", samples_to_bytes(samples));
    TempFile file("enc.wav");
    file.write(builder.finish());
    return read_wav(file.path()).error;
  };

  CHECK(try_encoding(0x0003, 32) == WavError::UnsupportedEncoding);  // float
  CHECK(try_encoding(0x0011, 16) ==
        WavError::UnsupportedEncoding);  // IMA ADPCM
  CHECK(try_encoding(0x0001, 24) == WavError::UnsupportedBitDepth);
  CHECK(try_encoding(0x0001, 8) == WavError::UnsupportedBitDepth);

  // The message tells the caller what to run, not merely that it failed.
  CHECK(std::string_view{conversion_hint(WavError::UnsupportedBitDepth)}.find(
            "ffmpeg") != std::string_view::npos);
}

TEST_CASE("structurally broken files are rejected by class") {
  {
    TempFile file("empty.wav");
    file.write({});
    CHECK(read_wav(file.path()).error == WavError::TooShort);
  }
  {
    TempFile file("notriff.wav");
    const std::vector<std::byte> bytes(64, std::byte{0x00});
    file.write(bytes);
    CHECK(read_wav(file.path()).error == WavError::NotRiff);
  }
  {
    // RIFF, but not a WAVE payload -- an AVI, for instance.
    std::vector<std::byte> bytes;
    for (char c : std::string("RIFF"))
      bytes.push_back(static_cast<std::byte>(c));
    bytes.insert(bytes.end(), 4, std::byte{0x20});
    for (char c : std::string("AVI "))
      bytes.push_back(static_cast<std::byte>(c));
    bytes.insert(bytes.end(), 32, std::byte{0});
    TempFile file("avi.wav");
    file.write(bytes);
    CHECK(read_wav(file.path()).error == WavError::NotWave);
  }
  {
    WavBuilder builder;
    builder.chunk("data", samples_to_bytes({1, 2}));
    TempFile file("nofmt.wav");
    file.write(builder.finish());
    CHECK(read_wav(file.path()).error == WavError::MissingFmtChunk);
  }
  {
    WavBuilder builder;
    const auto fmt = fmt_pcm(1, 48'000, 16);
    builder.chunk("fmt ", fmt);
    TempFile file("nodata.wav");
    file.write(builder.finish());
    CHECK(read_wav(file.path()).error == WavError::MissingDataChunk);
  }
  CHECK(read_wav("/nonexistent/path/nope.wav").error == WavError::CannotOpen);
}

TEST_CASE("a chunk claiming more bytes than the file holds is rejected") {
  // The bounds check that stops a hostile or corrupt size from being used as an
  // index. Distinguished from a corrupt header because a truncated recording is
  // a common, recoverable-looking situation.
  const auto fmt = fmt_pcm(1, 48'000, 16);
  {
    WavBuilder builder;
    builder.chunk("fmt ", fmt);
    builder.chunk("data", samples_to_bytes({1, 2}), /*declared=*/9999);
    TempFile file("liar.wav");
    file.write(builder.finish());
    CHECK(read_wav(file.path()).error == WavError::TruncatedData);
  }
  {
    WavBuilder builder;
    builder.chunk("fmt ", fmt);
    builder.chunk("junk", samples_to_bytes({1, 2}), /*declared=*/9999);
    TempFile file("liar2.wav");
    file.write(builder.finish());
    CHECK(read_wav(file.path()).error == WavError::MalformedChunk);
  }
}

TEST_CASE("a partial trailing frame is discarded rather than half-read") {
  // An interrupted stereo recording can end mid-frame. Keeping half a frame
  // would shift every subsequent channel assignment.
  const auto fmt = fmt_pcm(2, 48'000, 16);
  WavBuilder builder;
  builder.chunk("fmt ", fmt);
  builder.chunk("data", samples_to_bytes({10, 20, 30}));  // 1.5 stereo frames

  TempFile file("partial.wav");
  file.write(builder.finish());

  const auto read = read_wav(file.path());
  REQUIRE(read.ok());
  CHECK(read.info.frames == 1);
  CHECK(read.samples.size() == 2);
  CHECK(read.samples[0] == 10);
  CHECK(read.samples[1] == 20);
}

TEST_CASE("downmix averages channels instead of taking the first") {
  // A recording with the voice panned hard right, or with a dead left channel,
  // would come out silent if the first channel were simply taken.
  const std::vector<std::int16_t> right_only{0, 1000, 0, 2000, 0, 3000};
  const auto mono = downmix_to_mono(right_only, 2);
  REQUIRE(mono.size() == 3);
  CHECK(mono[0] == 500);
  CHECK(mono[1] == 1000);
  CHECK(mono[2] == 1500);
}

TEST_CASE("downmix does not overflow on many loud channels") {
  // Eight channels at full scale sum to 262136, far past int16. The sum has to
  // accumulate in a wider type.
  const std::vector<std::int16_t> loud(8, 32'767);
  const auto mono = downmix_to_mono(loud, 8);
  REQUIRE(mono.size() == 1);
  CHECK(mono[0] == 32'767);

  const std::vector<std::int16_t> quiet(8, -32'768);
  const auto negative = downmix_to_mono(quiet, 8);
  REQUIRE(negative.size() == 1);
  CHECK(negative[0] == -32'768);
}

TEST_CASE("downmix of mono is a copy, and of nothing is nothing") {
  const std::vector<std::int16_t> mono{1, 2, 3};
  CHECK(downmix_to_mono(mono, 1) == mono);
  CHECK(downmix_to_mono({}, 2).empty());
  CHECK(downmix_to_mono(mono, 0).empty());
}

TEST_CASE("writing rejects arguments it cannot honour") {
  TempFile file("bad.wav");
  const std::vector<std::int16_t> samples{1, 2, 3};
  CHECK(write_wav(file.path(), samples, 48'000, 0) ==
        WavError::UnsupportedChannelCount);
  CHECK(write_wav(file.path(), samples, 0, 1) == WavError::WriteFailed);
  // Three samples is not a whole number of stereo frames.
  CHECK(write_wav(file.path(), samples, 48'000, 2) == WavError::WriteFailed);
  CHECK(write_wav("/nonexistent/dir/x.wav", samples, 48'000, 1) ==
        WavError::CannotOpen);
}

TEST_CASE("an empty file is still a valid WAV") {
  TempFile file("silent.wav");
  REQUIRE(write_wav(file.path(), {}, 48'000, 1) == WavError::None);
  const auto read = read_wav(file.path());
  REQUIRE(read.ok());
  CHECK(read.info.frames == 0);
  CHECK(read.samples.empty());
  CHECK(read.info.sample_rate_hz == 48'000);
}

TEST_CASE("error names are distinct") {
  CHECK(std::string_view{to_string(WavError::None)} == "none");
  CHECK(std::string_view{to_string(WavError::NotRiff)} !=
        std::string_view{to_string(WavError::NotWave)});
  CHECK(std::string_view{to_string(WavError::TruncatedData)}.size() > 0);
}
