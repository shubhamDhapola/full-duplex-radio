// The media pipeline on the device: microphone to network, network to speaker.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>

#include "audio_engine.hpp"
#include "radio/clock.hpp"
#include "radio/endpoint.hpp"
#include "radio/jitter_buffer.hpp"
#include "radio/opus_codec.hpp"
#include "radio/proto.hpp"
#include "radio/spsc_ring.hpp"
#include "radio/udp_socket.hpp"

namespace fdradio {

// The same pipeline `radiobench wavloop` runs on the host, with the microphone
// and the speaker in place of the WAV files:
//
//   capture cb ──▶ PCM ring ──▶ network thread ──▶ encode ──▶ sendto
//   recvfrom  ──▶ packet ring ──▶ playback cb ──▶ jitter buffer ──▶ decode
//
// THREE THREADS, AND WHY THE WORK IS SPLIT WHERE IT IS
//
// Two of them are audio callbacks with hard deadlines; one is an ordinary
// thread that is allowed to block. What goes where is decided by that, not by
// convenience:
//
//   capture callback    copies PCM into a ring. Nothing else. Encoding is
//                       ~300 us and sendto() is a syscall, and neither belongs
//                       on a thread that must return in 2 ms.
//   network thread      encodes, sends, receives, parses. Blocking is fine
//                       here, so this is where everything that can block goes.
//   playback callback   drains the packet ring into the jitter buffer, pulls
//                       one frame, decodes. This looks like the most work of
//                       the three and it is the one place it has to happen --
//                       see below.
//
// WHY DECODING IS ON THE AUDIO CALLBACK AND SENDING IS NOT
//
// The asymmetry is deliberate and is the pull model of the jitter buffer
// (core/include/radio/jitter_buffer.hpp) carried onto the device. Playout is
// driven by the audio clock: the device asks for samples, so the frame is
// decoded in that call and there is no second clock to drift against. Moving
// the decode to the network thread would reintroduce exactly the drift the
// pull model exists to remove.
//
// Transmission has no such constraint. Nothing downstream of the microphone is
// waiting on a deadline, so the encode goes on the thread that is allowed to
// take its time.
//
// The jitter buffer is single-threaded by contract, and the contract is kept:
// only the playback callback touches it. The network thread hands whole
// datagrams over an SpscRing, which is the arrangement reorder_queue.hpp
// describes.
class Session : public Pipeline {
 public:
  struct Config {
    radio::net::Endpoint peer{};
    std::uint16_t local_port = 47000;
    std::int32_t bitrate_bps = 32'000;
    bool fec = false;
    int expected_loss_percent = 0;
    std::uint32_t target_delay_ms = 60;
  };

  struct Snapshot {
    bool running = false;
    bool transmitting = false;

    std::int64_t packets_sent = 0;
    std::int64_t bytes_sent = 0;
    std::int64_t send_failed = 0;
    std::int64_t encode_failed = 0;
    std::int64_t talkspurts = 0;

    std::int64_t datagrams_received = 0;
    std::int64_t rejected = 0;
    std::int64_t rx_ring_overflows = 0;

    // Straight from the jitter buffer, so the phone reports the same counters
    // the host benchmark does and the two can be compared directly.
    std::int64_t from_packet = 0;
    std::int64_t fec_recovered = 0;
    std::int64_t concealed = 0;
    std::int64_t silence = 0;
    std::int64_t late = 0;
    std::int64_t gaps = 0;
    std::int64_t duplicates = 0;
    std::int64_t depth_frames = 0;

    // Codec cost, measured rather than assumed. Kept as count/total/max
    // atomics instead of a Histogram because the two ends are written from
    // different threads and Histogram is not thread-safe; a racy percentile is
    // worse than an honest mean.
    std::int64_t encode_calls = 0;
    std::int64_t encode_total_us = 0;
    std::int64_t encode_max_us = 0;
    std::int64_t decode_calls = 0;
    std::int64_t decode_total_us = 0;
    std::int64_t decode_max_us = 0;

    std::int64_t tx_pcm_overflows = 0;
  };

  Session() = default;
  ~Session();

  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  [[nodiscard]] bool start(const Config& config) noexcept;
  void stop() noexcept;
  [[nodiscard]] bool running() const noexcept { return running_.load(); }

  // Push to talk. A talkspurt is a stream: a new stream_id, a fresh random
  // sequence and timestamp, TALKSPURT_START on the first packet and
  // TALKSPURT_END on the last (spec sections 3.2 and 3.1).
  void set_transmitting(bool on) noexcept;

  [[nodiscard]] Snapshot snapshot() const noexcept;
  [[nodiscard]] int last_error() const noexcept { return last_error_.load(); }

  // Pipeline, called from the Oboe callbacks.
  void on_capture(const std::int16_t* pcm,
                  std::int32_t frames) noexcept override;
  void on_playout(std::int16_t* pcm, std::int32_t frames) noexcept override;

 private:
  // A received datagram, whole, so the network thread never touches the jitter
  // buffer. Trivially copyable because SpscRing bulk-copies raw bytes.
  struct Datagram {
    radio::Micros arrival_us;
    std::uint16_t length;
    std::array<std::byte, radio::proto::kMaxDatagram> bytes;
  };

  // 64 datagrams is 1.28 s of audio: far beyond any reordering, sized so an
  // overflow means the playback callback stopped running rather than that the
  // network was untidy.
  static constexpr std::size_t kRxCapacity = 64;

  // 16384 samples is 340 ms of capture. The network thread drains it every few
  // milliseconds; this much headroom means an overflow is a starved thread.
  static constexpr std::size_t kTxPcmSamples = 16'384;

  void network_loop() noexcept;
  void transmit_available() noexcept;
  void receive_available() noexcept;

  radio::net::UdpSocket socket_;
  radio::audio::Encoder encoder_;
  radio::audio::JitterBuffer jitter_;

  Config config_{};

  radio::SpscRing<std::int16_t, kTxPcmSamples> tx_pcm_;
  radio::SpscRing<Datagram, kRxCapacity> rx_;

  std::thread network_;
  std::atomic<bool> running_{false};
  std::atomic<bool> transmitting_{false};

  // Owned by the network thread once started.
  std::uint32_t stream_id_ = 0;
  std::uint32_t sequence_ = 0;
  std::uint32_t timestamp_ = 0;
  bool talkspurt_open_ = false;

  // Playout staging. The device asks for 96 or 192 samples at a time while the
  // codec produces 960, so one decoded frame is held and served in pieces.
  // Owned by the playback callback alone.
  std::array<std::int16_t, radio::kFrameSamples> playout_{};
  std::size_t playout_used_ = radio::kFrameSamples;

  std::atomic<std::int64_t> packets_sent_{0};
  std::atomic<std::int64_t> bytes_sent_{0};
  std::atomic<std::int64_t> send_failed_{0};
  std::atomic<std::int64_t> encode_failed_{0};
  std::atomic<std::int64_t> talkspurts_{0};
  std::atomic<std::int64_t> datagrams_received_{0};
  std::atomic<std::int64_t> rejected_{0};
  std::atomic<std::int64_t> encode_calls_{0};
  std::atomic<std::int64_t> encode_total_us_{0};
  std::atomic<std::int64_t> encode_max_us_{0};
  std::atomic<std::int64_t> decode_calls_{0};
  std::atomic<std::int64_t> decode_total_us_{0};
  std::atomic<std::int64_t> decode_max_us_{0};
  std::atomic<std::int32_t> last_error_{0};
};

Session& session() noexcept;

}  // namespace fdradio
