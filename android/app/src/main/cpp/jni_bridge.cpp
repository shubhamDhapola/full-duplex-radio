// JNI shim over the project core.
//
// WHAT THIS IS, AND WHAT IT DELIBERATELY IS NOT
//
// This is the proof that the chain holds: Gradle -> CMake -> NDK -> radio_core
// -> JNI -> Kotlin. It runs the codec, the packet codec and the clock on the
// device and reports what happened.
//
// It also carries the audio boundary, and the shape of that is the point:
//
//   commands  UI thread -> engine.  start() and stop() open and close streams,
//             which blocks until the callback returns. Called from Kotlin, on
//             an ordinary thread, never from audio.
//   state     engine -> UI thread, by POLLING. The UI asks for a snapshot; the
//             audio callback never calls back into Java.
//
// The direction matters more than it looks. The obvious design has the callback
// notify the UI when something changes -- an underrun, a device disconnect --
// and that requires a JNI call from the audio thread. A JNI call can block on a
// class load, on the GC, or on acquiring the JNI lock, and a blocked audio
// callback is a click. There is no safe amount of JNI on that thread, so the
// direction is inverted: the callback only ever touches atomics, and whoever
// wants to know reads them.
//
// The cost is that the UI learns about an event up to one poll interval late.
// For a diagnostics screen that is free.
#include <jni.h>

#include <android/log.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <oboe/Oboe.h>

#include "audio_engine.hpp"
#include "radio/clock.hpp"
#include "radio/opus_codec.hpp"
#include "radio/proto.hpp"
#include "session.hpp"

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

  // Encode is timed here as well as in the session, and the pair is the point.
  // This one runs at start-up on an otherwise idle thread; the session's runs
  // on an ordinary-priority worker while two audio callbacks and the UI are
  // live. now_us() measures wall time, not CPU time, so the difference between
  // the two is contention rather than codec cost -- and quoting either one
  // alone would be misleading.
  const std::uint64_t encode_started = radio::now_ns();
  const auto encoded = encoder.encode(pcm, packet);
  const std::uint64_t encode_ns = radio::now_ns() - encode_started;
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
       number(encoded.bytes) + " bytes, encode " + number(encode_ns / 1000) +
           " us, decode " + number(elapsed_ns / 1000) + " us");
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

// ---------------------------------------------------------------- audio

extern "C" JNIEXPORT jboolean JNICALL Java_dev_fdradio_AudioEngine_nativeStart(
    JNIEnv* /*env*/, jobject /*thiz*/, jint capture, jboolean with_session) {
  const auto requested = static_cast<fdradio::AudioEngine::Capture>(capture);
  // A null pipeline means the microphone-to-speaker loopback; otherwise the
  // streams drive the network session.
  fdradio::Pipeline* pipeline =
      with_session == JNI_TRUE ? &fdradio::session() : nullptr;
  return fdradio::engine().start(requested, pipeline) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_dev_fdradio_AudioEngine_nativeStop(JNIEnv* /*env*/, jobject /*thiz*/) {
  fdradio::engine().stop();
}

// The whole diagnostic state as one long[].
//
// An array of primitives rather than a formatted string or a Java object,
// because this is polled several times a second: a string would mean building
// and parsing text on every poll, and constructing a Java object means
// FindClass and GetMethodID, which are the calls most likely to be slow. The
// field order is a contract with AudioEngine.kt and is asserted there by name,
// so a field inserted in the middle is a compile-time rename rather than a
// silently shifted column.
extern "C" JNIEXPORT jlongArray JNICALL
Java_dev_fdradio_AudioEngine_nativeSnapshot(JNIEnv* env, jobject /*thiz*/) {
  const fdradio::AudioEngine::Snapshot s = fdradio::engine().snapshot();

  const jlong values[] = {
      s.running ? 1 : 0,
      s.input_sample_rate,
      s.input_burst_frames,
      s.input_buffer_frames,
      s.input_capacity_frames,
      s.input_xruns,
      s.input_aaudio ? 1 : 0,
      s.input_low_latency ? 1 : 0,
      s.output_sample_rate,
      s.output_burst_frames,
      s.output_buffer_frames,
      s.output_capacity_frames,
      s.output_xruns,
      s.output_aaudio ? 1 : 0,
      s.output_low_latency ? 1 : 0,
      s.output_latency_us,
      s.ring_samples,
      s.ring_overflows,
      s.ring_underruns,
      s.input_callbacks,
      s.output_callbacks,
      s.worst_output_gap_us,
      s.last_error,
      s.capture,
      s.primed_samples,
  };
  constexpr jsize kCount =
      static_cast<jsize>(sizeof(values) / sizeof(values[0]));

  jlongArray array = env->NewLongArray(kCount);
  if (array == nullptr)
    return nullptr;  // OOM; the exception is already pending
  env->SetLongArrayRegion(array, 0, kCount, values);
  return array;
}

extern "C" JNIEXPORT jstring JNICALL
Java_dev_fdradio_AudioEngine_nativeErrorText(JNIEnv* env, jobject /*thiz*/,
                                             jint code) {
  return env->NewStringUTF(
      oboe::convertToText(static_cast<oboe::Result>(code)));
}

// ---------------------------------------------------------------- session

extern "C" JNIEXPORT jboolean JNICALL Java_dev_fdradio_Session_nativeStart(
    JNIEnv* env, jobject /*thiz*/, jstring peer_host, jint peer_port,
    jint local_port, jint bitrate, jboolean fec, jint expected_loss,
    jint target_delay_ms) {
  const char* host = env->GetStringUTFChars(peer_host, nullptr);
  if (host == nullptr) return JNI_FALSE;

  const auto peer =
      radio::net::Endpoint::parse(host, static_cast<std::uint16_t>(peer_port));
  env->ReleaseStringUTFChars(peer_host, host);

  // Numeric addresses only, by design: Endpoint::parse refuses hostnames
  // because getaddrinfo blocks, and nothing reachable from the media path may
  // block. Discovery is M3's job and hands over numeric addresses.
  if (!peer) return JNI_FALSE;

  fdradio::Session::Config config;
  config.peer = *peer;
  config.local_port = static_cast<std::uint16_t>(local_port);
  config.bitrate_bps = bitrate;
  config.fec = fec == JNI_TRUE;
  config.expected_loss_percent = expected_loss;
  config.target_delay_ms = static_cast<std::uint32_t>(target_delay_ms);

  return fdradio::session().start(config) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_dev_fdradio_Session_nativeStop(JNIEnv* /*env*/, jobject /*thiz*/) {
  fdradio::session().stop();
}

extern "C" JNIEXPORT void JNICALL
Java_dev_fdradio_Session_nativeSetTransmitting(JNIEnv* /*env*/,
                                               jobject /*thiz*/, jboolean on) {
  fdradio::session().set_transmitting(on == JNI_TRUE);
}

extern "C" JNIEXPORT jlongArray JNICALL
Java_dev_fdradio_Session_nativeSnapshot(JNIEnv* env, jobject /*thiz*/) {
  const fdradio::Session::Snapshot s = fdradio::session().snapshot();

  const jlong values[] = {
      s.running ? 1 : 0,
      s.transmitting ? 1 : 0,
      s.packets_sent,
      s.bytes_sent,
      s.send_failed,
      s.encode_failed,
      s.talkspurts,
      s.datagrams_received,
      s.rejected,
      s.rx_ring_overflows,
      s.from_packet,
      s.fec_recovered,
      s.concealed,
      s.silence,
      s.late,
      s.gaps,
      s.duplicates,
      s.depth_frames,
      s.encode_calls,
      s.encode_total_us,
      s.encode_max_us,
      s.decode_calls,
      s.decode_total_us,
      s.decode_max_us,
      s.tx_pcm_overflows,
      s.link,
      s.probes_sent,
      s.pongs_received,
      s.probes_timed_out,
      s.probes_stale,
      s.probes_bad_echo,
      s.probes_impossible,
      s.pongs_sent,
      s.rtt_last_us,
      s.rtt_best_us,
      s.offset_us,
      s.offset_uncertainty_us,
      s.rx_jitter_us,
      s.rx_received,
      s.rx_lost,
      s.rx_duplicates,
      s.rx_reordered,
      s.unhandled,
  };
  constexpr jsize kCount =
      static_cast<jsize>(sizeof(values) / sizeof(values[0]));

  jlongArray array = env->NewLongArray(kCount);
  if (array == nullptr) return nullptr;
  env->SetLongArrayRegion(array, 0, kCount, values);
  return array;
}
