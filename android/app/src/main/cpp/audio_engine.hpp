// Oboe capture and playback, and the rules that hold on the audio callback.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

#include <oboe/Oboe.h>

#include "radio/spsc_ring.hpp"

namespace fdradio {

// What the audio callbacks hand their work to.
//
// A virtual call on the audio callback is fine -- it allocates nothing and
// blocks on nothing -- and it is what lets the Oboe layer stay ignorant of
// whether it is driving a loopback or a network session. Everything an
// implementation does inside these two methods is bound by the callback
// contract described on AudioEngine below.
class Pipeline {
 public:
  virtual void on_capture(const std::int16_t* pcm,
                          std::int32_t frames) noexcept = 0;
  virtual void on_playout(std::int16_t* pcm, std::int32_t frames) noexcept = 0;

 protected:
  ~Pipeline() = default;
};

// Opens one input stream and one output stream in low-latency mode and moves
// PCM between them through a lock-free ring.
//
// WHY TWO INDEPENDENT STREAMS RATHER THAN OBOE'S FullDuplexStream
//
// Oboe ships a duplex helper that drives the input from the output callback, so
// one callback does both. It is the right answer when you genuinely need the
// microphone and the speaker sample-locked -- echo cancellation, for instance,
// which is M7.
//
// It is the wrong shape for this application. In the finished product capture
// goes to the network and playback comes from the jitter buffer; the two sides
// never meet, and pretending they are one stream would mean unpicking it later.
// So the structure here is the structure that ships: two callbacks, each owning
// one direction, exchanging data with the rest of the program through an
// SpscRing. Wiring the microphone straight to the speaker is temporary, and is
// only here so that both halves can be proved on a device before there is a
// network to carry anything.
//
// WHAT AN AUDIO CALLBACK MAY NOT DO
//
// The callback runs on a thread the OS gives us, with a hard deadline: the
// hardware consumes a buffer whether or not we filled it, and a late return is
// an audible click. So inside onAudioReady there is:
//
//   - no allocation: new, std::string, std::vector, std::function
//   - no locks: a mutex held by any other thread is an unbounded wait
//   - no syscalls: file I/O, logging, sleeping
//   - no JNI: a JNI call can block on a class load or on the GC
//   - no unbounded loops
//
// Every one of those is easy to write by accident and none of them fail
// loudly -- they produce an occasional click that is indistinguishable from
// network jitter by the time anyone hears it. The ring, the atomics and the
// fixed-size buffers below exist to make the rule keepable rather than merely
// stated.
class AudioEngine : public oboe::AudioStreamDataCallback,
                    public oboe::AudioStreamErrorCallback {
 public:
  // 8192 samples is about 170 ms at 48 kHz: far more than the few bursts the
  // loopback needs, sized so that an overflow means a genuinely starved thread
  // rather than ordinary scheduling. Fixed and preallocated, because growing it
  // would mean allocating on the callback.
  static constexpr std::size_t kRingSamples = 8192;

  // What the UI polls. Plain integers, copied out under no lock: every field is
  // an atomic written by one thread, so a snapshot can be very slightly
  // inconsistent between fields and that is fine for a diagnostics screen. The
  // alternative -- a mutex the callback must take -- is not.
  struct Snapshot {
    bool running = false;

    std::int32_t input_sample_rate = 0;
    std::int32_t input_burst_frames = 0;
    std::int32_t input_buffer_frames = 0;
    std::int32_t input_capacity_frames = 0;
    std::int64_t input_xruns = 0;
    bool input_aaudio = false;
    bool input_low_latency = false;

    std::int32_t output_sample_rate = 0;
    std::int32_t output_burst_frames = 0;
    std::int32_t output_buffer_frames = 0;
    std::int32_t output_capacity_frames = 0;
    std::int64_t output_xruns = 0;
    bool output_aaudio = false;
    bool output_low_latency = false;

    // Oboe's own estimate of output latency, which on AAudio comes from the
    // hardware timestamps rather than from arithmetic over buffer sizes.
    std::int64_t output_latency_us = 0;

    std::int64_t ring_samples = 0;
    std::int64_t ring_overflows = 0;  // capture outran playback
    std::int64_t ring_underruns = 0;  // playback found the ring empty

    // Samples thrown away on the first output callback to remove the head
    // start capture got while the output stream was still opening. Measured at
    // 5760 samples -- 120 ms -- on a Redmi Note 9 Pro, which is latency that
    // would otherwise have stood for the whole session.
    std::int64_t primed_samples = 0;

    std::int64_t input_callbacks = 0;
    std::int64_t output_callbacks = 0;

    // Longest observed gap between successive output callbacks. The audio
    // deadline made visible: if this exceeds the burst duration the device
    // missed one, whatever the xrun counter says.
    std::int64_t worst_output_gap_us = 0;

    std::int32_t last_error = 0;
    std::int32_t capture = 0;  // the Capture actually requested
  };

  // Which capture chain to ask the platform for. This is not a tuning knob, it
  // is a documented trade-off, and it was found by measurement rather than
  // chosen: on a Redmi Note 9 Pro, VoiceCommunication gets 960-frame bursts and
  // PerformanceMode::None, while VoiceRecognition and Unprocessed get the
  // low-latency path. The platform will not give you its echo canceller and its
  // fast capture path at the same time.
  //
  // That matters well beyond this milestone. M7 has to decide whether to adopt
  // the platform's echo cancellation or build one, and this is the cost side of
  // that decision made concrete -- so the choice is exposed and measured rather
  // than hardcoded to whichever happened to work first.
  enum class Capture : std::int32_t {
    VoiceCommunication = 0,  // platform AEC and noise suppression
    VoiceRecognition = 1,    // usually no AEC, usually the fast path
    Unprocessed = 2,         // rawest the device offers
  };

  AudioEngine() = default;
  ~AudioEngine() override;

  AudioEngine(const AudioEngine&) = delete;
  AudioEngine& operator=(const AudioEngine&) = delete;

  // Opens and starts both streams. Safe to call when already running, in which
  // case it does nothing. Not callable from the audio callback.
  // `pipeline` may be null, in which case capture is wired straight to
  // playback through the ring -- the loopback that proved the audio path
  // before there was anything else to do with the samples. It is kept because
  // it is the shortest test that distinguishes "the device is broken" from
  // "our pipeline is broken".
  [[nodiscard]] bool start(Capture capture, Pipeline* pipeline) noexcept;
  void stop() noexcept;
  [[nodiscard]] bool running() const noexcept { return running_.load(); }

  [[nodiscard]] Snapshot snapshot() const noexcept;

  // oboe::AudioStreamDataCallback
  oboe::DataCallbackResult onAudioReady(oboe::AudioStream* stream,
                                        void* audio_data,
                                        std::int32_t frames) override;

  // oboe::AudioStreamErrorCallback. Called on Oboe's own thread, never on the
  // audio callback, so it is allowed to do real work.
  void onErrorAfterClose(oboe::AudioStream* stream,
                         oboe::Result error) override;

 private:
  [[nodiscard]] bool open_stream(
      oboe::Direction direction, Capture capture,
      std::shared_ptr<oboe::AudioStream>& out) noexcept;

  std::shared_ptr<oboe::AudioStream> input_;
  std::shared_ptr<oboe::AudioStream> output_;

  // Capture pushes, playback pops. Exactly one thread on each end, which is the
  // constraint that makes SpscRing correct -- and the reason the loopback is
  // wired this way rather than through a shared buffer.
  radio::SpscRing<std::int16_t, kRingSamples> ring_;

  std::atomic<bool> running_{false};
  std::atomic<std::int64_t> underruns_{0};
  std::atomic<std::int64_t> input_callbacks_{0};
  std::atomic<std::int64_t> output_callbacks_{0};
  std::atomic<std::int64_t> worst_output_gap_us_{0};
  std::atomic<std::int64_t> last_output_callback_us_{0};
  std::atomic<std::int64_t> primed_samples_{0};
  std::atomic<std::int32_t> last_error_{0};
  std::atomic<std::int32_t> capture_{0};

  // Touched only by the output callback, which is the ring's sole consumer.
  // Not atomic for that reason: making it atomic would suggest another thread
  // reads it, and nothing else may.
  bool primed_ = false;

  // Set before the streams start and cleared after they stop, so the callbacks
  // never see it change under them.
  Pipeline* pipeline_ = nullptr;
};

// One engine per process. The audio device is a single resource and two engines
// competing for it is never what anyone meant.
AudioEngine& engine() noexcept;

}  // namespace fdradio
