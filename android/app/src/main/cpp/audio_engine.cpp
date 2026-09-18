#include "audio_engine.hpp"

#include <algorithm>
#include <cstring>

#include <android/log.h>

#include "radio/clock.hpp"

namespace fdradio {

namespace {

constexpr const char* kTag = "fdradio";

// The wire format is 48 kHz mono (spec section 3.3), so the device is asked for
// exactly that. Oboe will resample if the hardware disagrees; the diagnostics
// report what was actually granted, because a resampled stream is a different
// latency profile from a native one.
constexpr std::int32_t kSampleRate = 48'000;
constexpr std::int32_t kChannels = 1;

oboe::InputPreset to_preset(AudioEngine::Capture capture) noexcept {
  switch (capture) {
    case AudioEngine::Capture::VoiceRecognition:
      return oboe::InputPreset::VoiceRecognition;
    case AudioEngine::Capture::Unprocessed:
      return oboe::InputPreset::Unprocessed;
    case AudioEngine::Capture::VoiceCommunication:
      break;
  }
  return oboe::InputPreset::VoiceCommunication;
}

const char* to_text(AudioEngine::Capture capture) noexcept {
  switch (capture) {
    case AudioEngine::Capture::VoiceRecognition:
      return "VoiceRecognition";
    case AudioEngine::Capture::Unprocessed:
      return "Unprocessed";
    case AudioEngine::Capture::VoiceCommunication:
      break;
  }
  return "VoiceCommunication";
}

}  // namespace

AudioEngine& engine() noexcept {
  static AudioEngine instance;
  return instance;
}

AudioEngine::~AudioEngine() { stop(); }

bool AudioEngine::open_stream(
    oboe::Direction direction, Capture capture,
    std::shared_ptr<oboe::AudioStream>& out) noexcept {
  oboe::AudioStreamBuilder builder;

  builder
      .setDirection(direction)
      // The two settings that make this low latency, and they are requests
      // rather than guarantees -- what was granted is read back afterwards.
      ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
      ->setSharingMode(oboe::SharingMode::Exclusive)
      ->setSampleRate(kSampleRate)
      ->setChannelCount(kChannels)
      // I16 rather than float: the codec and the whole core work in int16, so
      // asking for float would add a conversion on the callback for nothing.
      ->setFormat(oboe::AudioFormat::I16)
      ->setSampleRateConversionQuality(
          oboe::SampleRateConversionQuality::Medium)
      ->setDataCallback(this)
      ->setErrorCallback(this);

  if (direction == oboe::Direction::Input) {
    // The preset decides whether the platform inserts its voice-processing
    // chain, and on the devices measured so far that decides whether capture
    // gets the low-latency path at all. See the note on Capture.
    builder.setInputPreset(to_preset(capture));
  } else {
    builder.setUsage(oboe::Usage::VoiceCommunication);
  }

  const oboe::Result result = builder.openStream(out);
  if (result != oboe::Result::OK) {
    last_error_.store(static_cast<std::int32_t>(result));
    __android_log_print(
        ANDROID_LOG_ERROR, kTag, "open %s failed: %s",
        direction == oboe::Direction::Input ? "input" : "output",
        oboe::convertToText(result));
    return false;
  }

  // Two bursts is the standard low-latency starting point: one being consumed
  // by the hardware, one being filled. Smaller risks an underrun on every
  // scheduling hiccup; larger is latency paid for nothing.
  const std::int32_t burst = out->getFramesPerBurst();
  (void)out->setBufferSizeInFrames(burst * 2);

  __android_log_print(
      ANDROID_LOG_INFO, kTag,
      "%s stream [%s]: api=%s rate=%d burst=%d buffer=%d perf=%s sharing=%s",
      direction == oboe::Direction::Input ? "input" : "output",
      direction == oboe::Direction::Input ? to_text(capture) : "-",
      oboe::convertToText(out->getAudioApi()), out->getSampleRate(), burst,
      out->getBufferSizeInFrames(),
      oboe::convertToText(out->getPerformanceMode()),
      oboe::convertToText(out->getSharingMode()));
  return true;
}

bool AudioEngine::start(Capture capture, Pipeline* pipeline) noexcept {
  if (running_.load()) return true;
  pipeline_ = pipeline;

  ring_.reset();
  underruns_.store(0);
  input_callbacks_.store(0);
  output_callbacks_.store(0);
  worst_output_gap_us_.store(0);
  last_output_callback_us_.store(0);
  last_error_.store(0);
  capture_.store(static_cast<std::int32_t>(capture));

  if (!open_stream(oboe::Direction::Input, capture, input_)) {
    stop();
    return false;
  }
  if (!open_stream(oboe::Direction::Output, capture, output_)) {
    stop();
    return false;
  }

  // Input first, so the ring has something in it before playback pulls and the
  // run does not open with a burst of underruns that were really just start-up
  // ordering.
  //
  // The cost is that capture gets a head start: opening the output stream took
  // 141 ms on the device measured, and every one of those samples sat in the
  // ring for the rest of the session -- 120 ms of standing latency that no
  // counter would have called a fault. The output callback throws it away on
  // its first call; see `primed_`.
  oboe::Result result = input_->requestStart();
  if (result != oboe::Result::OK) {
    last_error_.store(static_cast<std::int32_t>(result));
    stop();
    return false;
  }
  result = output_->requestStart();
  if (result != oboe::Result::OK) {
    last_error_.store(static_cast<std::int32_t>(result));
    stop();
    return false;
  }

  primed_ = false;
  primed_samples_.store(0);
  running_.store(true);
  return true;
}

void AudioEngine::stop() noexcept {
  running_.store(false);

  // Output first, so playback stops pulling from a ring the capture side is
  // about to stop filling. close() waits for the callback to return, which is
  // why nothing here may be called from the callback itself.
  if (output_) {
    (void)output_->requestStop();
    (void)output_->close();
    output_.reset();
  }
  if (input_) {
    (void)input_->requestStop();
    (void)input_->close();
    input_.reset();
  }

  // After close(), which waits for the callbacks to return: clearing it any
  // earlier would pull the pipeline out from under a callback still running.
  pipeline_ = nullptr;
}

oboe::DataCallbackResult AudioEngine::onAudioReady(oboe::AudioStream* stream,
                                                   void* audio_data,
                                                   std::int32_t frames) {
  // Mono, so frames and samples are the same count. Stated rather than assumed
  // because the day this becomes stereo, every length below is wrong by two.
  const auto samples = static_cast<std::size_t>(frames);

  if (stream->getDirection() == oboe::Direction::Input) {
    input_callbacks_.fetch_add(1, std::memory_order_relaxed);

    const auto* pcm = static_cast<const std::int16_t*>(audio_data);
    if (pipeline_ != nullptr) {
      pipeline_->on_capture(pcm, frames);
      return oboe::DataCallbackResult::Continue;
    }
    // A short write means the ring is full: the consumer is further behind than
    // the buffer is deep. Counted by the ring itself; nothing to do here but
    // carry on, because blocking is the one thing forbidden.
    (void)ring_.push_bulk(pcm, samples);
    return oboe::DataCallbackResult::Continue;
  }

  output_callbacks_.fetch_add(1, std::memory_order_relaxed);

  // The audio deadline, made measurable. now_us() is a clock_gettime on
  // CLOCK_MONOTONIC_RAW -- a vDSO read on Android, so no syscall and safe here.
  const radio::Micros now = radio::now_us();
  const std::int64_t previous =
      last_output_callback_us_.load(std::memory_order_relaxed);
  if (previous != 0) {
    const std::int64_t gap = static_cast<std::int64_t>(now) - previous;
    std::int64_t worst = worst_output_gap_us_.load(std::memory_order_relaxed);
    while (gap > worst && !worst_output_gap_us_.compare_exchange_weak(
                              worst, gap, std::memory_order_relaxed)) {
    }
  }
  last_output_callback_us_.store(static_cast<std::int64_t>(now),
                                 std::memory_order_relaxed);

  auto* pcm = static_cast<std::int16_t*>(audio_data);

  if (pipeline_ != nullptr) {
    pipeline_->on_playout(pcm, frames);
    return oboe::DataCallbackResult::Continue;
  }

  // First call: drop everything capture accumulated while this stream was
  // still opening, keeping one cushion burst. Done here rather than in start()
  // because discard() moves the read index, and the read index belongs to the
  // consumer -- doing it from the starting thread would make this a two-reader
  // ring and forfeit the lock-free argument entirely.
  if (!primed_) {
    primed_ = true;
    const std::size_t keep = samples * 2;
    const std::size_t held = ring_.size();
    if (held > keep) {
      const std::size_t dropped = ring_.discard(held - keep);
      primed_samples_.store(static_cast<std::int64_t>(dropped),
                            std::memory_order_relaxed);
    }
  }

  const std::size_t got = ring_.pop_bulk(pcm, samples);

  if (got < samples) {
    // Silence rather than stale audio, and counted. This is exactly what the
    // jitter buffer's concealment path will replace once there is a network on
    // the other end -- here it only has to be honest about the shortfall.
    std::fill_n(pcm + got, samples - got, std::int16_t{0});
    underruns_.fetch_add(1, std::memory_order_relaxed);
  }

  return oboe::DataCallbackResult::Continue;
}

void AudioEngine::onErrorAfterClose(oboe::AudioStream* stream,
                                    oboe::Result error) {
  last_error_.store(static_cast<std::int32_t>(error));
  running_.store(false);
  // Disconnection is normal, not exceptional: it is what happens when a headset
  // is unplugged or a call arrives. Recovery belongs with the session handling
  // in M3; reporting it honestly is what this milestone owes.
  __android_log_print(
      ANDROID_LOG_WARN, kTag, "%s stream closed: %s",
      stream->getDirection() == oboe::Direction::Input ? "input" : "output",
      oboe::convertToText(error));
}

AudioEngine::Snapshot AudioEngine::snapshot() const noexcept {
  Snapshot out;
  out.running = running_.load();

  if (input_) {
    out.input_sample_rate = input_->getSampleRate();
    out.input_burst_frames = input_->getFramesPerBurst();
    out.input_buffer_frames = input_->getBufferSizeInFrames();
    out.input_capacity_frames = input_->getBufferCapacityInFrames();
    out.input_xruns = input_->getXRunCount().value();
    out.input_aaudio = input_->getAudioApi() == oboe::AudioApi::AAudio;
    out.input_low_latency =
        input_->getPerformanceMode() == oboe::PerformanceMode::LowLatency;
  }

  if (output_) {
    out.output_sample_rate = output_->getSampleRate();
    out.output_burst_frames = output_->getFramesPerBurst();
    out.output_buffer_frames = output_->getBufferSizeInFrames();
    out.output_capacity_frames = output_->getBufferCapacityInFrames();
    out.output_xruns = output_->getXRunCount().value();
    out.output_aaudio = output_->getAudioApi() == oboe::AudioApi::AAudio;
    out.output_low_latency =
        output_->getPerformanceMode() == oboe::PerformanceMode::LowLatency;

    const auto latency = output_->calculateLatencyMillis();
    if (latency) {
      out.output_latency_us =
          static_cast<std::int64_t>(latency.value() * 1000.0);
    }
  }

  out.ring_samples = static_cast<std::int64_t>(ring_.size());
  out.ring_overflows = static_cast<std::int64_t>(ring_.overflows());
  out.ring_underruns = underruns_.load();
  out.primed_samples = primed_samples_.load();
  out.input_callbacks = input_callbacks_.load();
  out.output_callbacks = output_callbacks_.load();
  out.worst_output_gap_us = worst_output_gap_us_.load();
  out.last_error = last_error_.load();
  out.capture = capture_.load();
  return out;
}

}  // namespace fdradio
