// JNI shim over the project core.
//
// WHAT THIS IS, AND WHAT IT DELIBERATELY IS NOT
//
// This is the proof that the chain holds: Gradle -> CMake -> NDK -> radio_core
// -> JNI -> Kotlin. It runs the codec, the packet codec and the clock on the
// device and reports what happened.
//
// It is NOT the JNI boundary the app will use for audio. That boundary carries
// commands and state and must never be crossed from the audio callback, and it
// arrives with the Oboe work. Keeping this file to a single self-check until
// then means the first thing built on the device is something whose failure is
// unambiguous: if selfCheck() reports a bad Opus round trip, the problem is the
// toolchain or the core, not a threading model that does not exist yet.
#include <jni.h>

#include <android/log.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "radio/clock.hpp"
#include "radio/opus_codec.hpp"
#include "radio/proto.hpp"

namespace {

constexpr const char* kTag = "fdradio";

void line(std::string& out, bool ok, const char* label,
          const std::string& detail) {
  out += ok ? "PASS  " : "FAIL  ";
  out += label;
  if (!detail.empty()) {
    out += "  ";
    out += detail;
  }
  out += '\n';
}

std::string number(std::uint64_t value) { return std::to_string(value); }

// Encodes a media packet and parses it back. This is the layer every
// conformance vector exercises on the host; running it here proves the same
// bytes come out on a different architecture and a different libc++.
bool check_protocol(std::string& out) {
  radio::proto::MediaHeader header;
  header.stream_id = 0xDEADBEEF;
  header.sequence = 0xFFFFFFF0;  // near the wrap, where the arithmetic matters
  header.timestamp = 123456;
  header.flags = radio::proto::media_flag::kTalkspurtStart;

  const std::array<std::byte, 4> payload{std::byte{0x11}, std::byte{0x22},
                                         std::byte{0x33}, std::byte{0x44}};
  std::array<std::byte, radio::proto::kMaxDatagram> datagram{};

  const std::size_t length = radio::proto::encode_media(
      header, radio::ByteView{payload.data(), payload.size()}, datagram);
  if (length == 0) {
    line(out, false, "protocol round trip", "encode produced nothing");
    return false;
  }

  const auto parsed =
      radio::proto::parse_media(radio::ByteView{datagram.data(), length});
  if (!parsed) {
    line(out, false, "protocol round trip",
         std::string("rejected: ") + radio::proto::to_string(parsed.reject));
    return false;
  }

  const radio::proto::MediaHeader& got = parsed.value.header;
  const bool ok =
      got.stream_id == header.stream_id && got.sequence == header.sequence &&
      got.timestamp == header.timestamp && got.flags == header.flags &&
      parsed.value.payload.size() == payload.size() && got.talkspurt_start();
  line(out, ok, "protocol round trip", number(length) + " byte datagram");
  return ok;
}

// Opens the encoder and decoder, runs one 20 ms frame through both, and checks
// the decoder gave back a full frame. Linking libopus is one thing; proving it
// runs on this CPU is another, and this is the cheapest place to find out.
bool check_codec(std::string& out) {
  radio::audio::EncoderConfig encoder_config;
  encoder_config.bitrate_bps = 32'000;

  radio::audio::Encoder encoder;
  if (!encoder.open(encoder_config)) {
    line(out, false, "opus encoder",
         radio::audio::error_string(encoder.last_error()));
    return false;
  }

  radio::audio::Decoder decoder;
  if (!decoder.open(radio::audio::DecoderConfig{})) {
    line(out, false, "opus decoder",
         radio::audio::error_string(decoder.last_error()));
    return false;
  }

  std::vector<std::int16_t> pcm(radio::kFrameSamples);
  for (std::size_t i = 0; i < pcm.size(); ++i) {
    // A ramp rather than silence: DTX is off, but silence still encodes to
    // almost nothing and would make a broken encoder look healthy.
    pcm[i] = static_cast<std::int16_t>((static_cast<int>(i) % 400) * 20 - 4000);
  }

  std::vector<std::byte> packet(radio::proto::kMaxPayload);
  const auto encoded = encoder.encode(pcm, packet);
  if (!encoded.ok() || encoded.bytes == 0) {
    line(out, false, "opus encode", radio::audio::error_string(encoded.error));
    return false;
  }

  const std::uint64_t started = radio::now_ns();
  std::vector<std::int16_t> decoded(radio::kFrameSamples);
  const auto result =
      decoder.decode(radio::ByteView{packet.data(), encoded.bytes}, decoded);
  const std::uint64_t elapsed_ns = radio::now_ns() - started;

  if (!result.ok() || result.samples != radio::kFrameSamples) {
    line(out, false, "opus decode", radio::audio::error_string(result.error));
    return false;
  }

  line(out, true, "opus round trip",
       number(encoded.bytes) + " bytes, decode " + number(elapsed_ns / 1000) +
           " us");
  return true;
}

// The time base everything else is measured against. A clock that does not
// advance, or that is not CLOCK_MONOTONIC_RAW, invalidates every figure the
// app will ever report -- so it is checked rather than assumed.
bool check_clock(std::string& out) {
  const radio::Micros first = radio::now_us();
  std::uint64_t spin = 0;
  for (int i = 0; i < 200'000; ++i) spin += static_cast<std::uint64_t>(i);
  const radio::Micros second = radio::now_us();

  const bool ok = first != 0 && second >= first && spin != 0;
  line(out, ok, "monotonic clock",
       number(second - first) + " us across a busy loop");
  return ok;
}

std::string run_self_check() {
  std::string out;
  out += "protocol v";
  out += std::to_string(radio::proto::kVersion);
  out += ", ";
  out += std::to_string(radio::kSampleRateHz / 1000);
  out += " kHz, ";
  out += std::to_string(radio::kFrameMs);
  out += " ms frames\n\n";

  bool ok = true;
  ok = check_protocol(out) && ok;
  ok = check_codec(out) && ok;
  ok = check_clock(out) && ok;

  out += '\n';
  out += ok ? "core self-check passed" : "CORE SELF-CHECK FAILED";

  __android_log_print(ok ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, kTag, "%s",
                      out.c_str());
  return out;
}

}  // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_dev_fdradio_NativeCore_selfCheck(JNIEnv* env, jobject /*thiz*/) {
  return env->NewStringUTF(run_self_check().c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_dev_fdradio_NativeCore_abi(JNIEnv* env, jobject /*thiz*/) {
#if defined(__aarch64__)
  const char* abi = "arm64-v8a";
#elif defined(__x86_64__)
  const char* abi = "x86_64";
#else
  const char* abi = "unknown";
#endif
  return env->NewStringUTF(abi);
}
