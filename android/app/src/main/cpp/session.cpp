#include "session.hpp"

#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <random>

#include <android/log.h>

namespace fdradio {

namespace {

constexpr const char* kTag = "fdradio";

// How long the network thread waits on the socket before looking at the
// transmit ring again. Short enough that a frame ready to send waits at most
// this long, long enough that an idle session is not a spin loop.
constexpr int kPollMs = 2;

void note_time(std::atomic<std::int64_t>& calls,
               std::atomic<std::int64_t>& total, std::atomic<std::int64_t>& max,
               std::int64_t elapsed_us) noexcept {
  calls.fetch_add(1, std::memory_order_relaxed);
  total.fetch_add(elapsed_us, std::memory_order_relaxed);
  std::int64_t seen = max.load(std::memory_order_relaxed);
  while (
      elapsed_us > seen &&
      !max.compare_exchange_weak(seen, elapsed_us, std::memory_order_relaxed)) {
  }
}

}  // namespace

Session& session() noexcept {
  static Session instance;
  return instance;
}

Session::~Session() { stop(); }

bool Session::start(const Config& config) noexcept {
  if (running_.load()) return true;

  config_ = config;

  radio::audio::EncoderConfig encoder_config;
  encoder_config.bitrate_bps = config.bitrate_bps;
  encoder_config.inband_fec = config.fec;
  encoder_config.expected_loss_percent = config.expected_loss_percent;
  if (!encoder_.open(encoder_config)) {
    last_error_.store(encoder_.last_error());
    __android_log_print(ANDROID_LOG_ERROR, kTag, "encoder open failed: %s",
                        radio::audio::error_string(encoder_.last_error()));
    return false;
  }

  radio::audio::JitterBuffer::Config jitter_config;
  jitter_config.target_delay_us =
      static_cast<radio::Micros>(config.target_delay_ms) * 1000u;
  jitter_config.fec = config.fec;
  if (!jitter_.open(jitter_config)) {
    encoder_.close();
    return false;
  }

  radio::net::UdpSocket::Options socket_options;
  socket_options.family = radio::net::Endpoint::Family::V4;
  socket_options.port = config.local_port;
  if (!socket_.open(socket_options)) {
    last_error_.store(socket_.last_error());
    __android_log_print(ANDROID_LOG_ERROR, kTag, "socket open failed: %d",
                        socket_.last_error());
    encoder_.close();
    jitter_.close();
    return false;
  }

  tx_pcm_.reset();
  rx_.reset();
  playout_used_ = radio::kFrameSamples;
  talkspurt_open_ = false;
  transmitting_.store(false);

  packets_sent_.store(0);
  bytes_sent_.store(0);
  send_failed_.store(0);
  encode_failed_.store(0);
  talkspurts_.store(0);
  datagrams_received_.store(0);
  rejected_.store(0);
  encode_calls_.store(0);
  encode_total_us_.store(0);
  encode_max_us_.store(0);
  decode_calls_.store(0);
  decode_total_us_.store(0);
  decode_max_us_.store(0);

  running_.store(true);
  network_ = std::thread([this] { network_loop(); });

  __android_log_print(
      ANDROID_LOG_INFO, kTag, "session up: local %u -> peer %s, %d bps, fec %s",
      static_cast<unsigned>(config.local_port), config.peer.to_string().c_str(),
      config.bitrate_bps, config.fec ? "on" : "off");
  return true;
}

void Session::stop() noexcept {
  if (!running_.exchange(false)) return;
  if (network_.joinable()) network_.join();

  socket_.close();
  encoder_.close();
  jitter_.close();
}

void Session::set_transmitting(bool on) noexcept {
  transmitting_.store(on, std::memory_order_relaxed);
}

void Session::on_capture(const std::int16_t* pcm,
                         std::int32_t frames) noexcept {
  // Copy and return. Everything expensive happens on the network thread.
  (void)tx_pcm_.push_bulk(pcm, static_cast<std::size_t>(frames));
}

void Session::on_playout(std::int16_t* pcm, std::int32_t frames) noexcept {
  // This callback owns the jitter buffer, so it is also the only place
  // datagrams may enter it. Draining here rather than on the network thread is
  // what keeps the buffer single-threaded.
  Datagram datagram;
  while (rx_.pop(datagram)) {
    const auto parsed = radio::proto::parse_media(
        radio::ByteView{datagram.bytes.data(), datagram.length});
    if (!parsed) {
      rejected_.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    (void)jitter_.push(parsed.value, datagram.arrival_us);
  }

  auto remaining = static_cast<std::size_t>(frames);
  std::size_t written = 0;

  while (remaining > 0) {
    if (playout_used_ >= radio::kFrameSamples) {
      // The device asked for more audio than the last decoded frame still
      // holds, so decode the next one. This is the pull model: the audio clock
      // is what advances the playout timeline, and the decode happens in the
      // call that needs it.
      const radio::Micros started = radio::now_us();
      const auto pulled = jitter_.pull(playout_, radio::now_us());
      const auto elapsed = static_cast<std::int64_t>(radio::now_us() - started);
      note_time(decode_calls_, decode_total_us_, decode_max_us_, elapsed);
      (void)pulled;
      playout_used_ = 0;
    }

    const std::size_t take =
        std::min(remaining, radio::kFrameSamples - playout_used_);
    std::copy_n(playout_.data() + playout_used_, take, pcm + written);
    playout_used_ += take;
    written += take;
    remaining -= take;
  }
}

void Session::transmit_available() noexcept {
  // One Opus frame at a time, and only whole frames: a short frame would
  // desynchronise the timestamp on the wire from the audio actually sent.
  std::array<std::int16_t, radio::kFrameSamples> frame{};
  std::array<std::byte, radio::proto::kMaxDatagram> datagram{};
  std::array<std::byte, radio::proto::kMaxPayload> payload{};

  while (tx_pcm_.size() >= radio::kFrameSamples) {
    const std::size_t got = tx_pcm_.pop_bulk(frame.data(), frame.size());
    if (got < frame.size()) break;

    const bool sending = transmitting_.load(std::memory_order_relaxed);
    bool first_of_talkspurt = false;

    if (sending && !talkspurt_open_) {
      // A talkspurt is a stream. Spec section 3.3 wants random starting values,
      // and section 3.2 wants a new stream id per talkspurt so a straggler from
      // the previous one lands somewhere the receiver can recognise.
      std::random_device entropy;
      std::mt19937 generator{entropy()};
      std::uniform_int_distribution<std::uint32_t> any32{0, 0xFFFFFFFFu};
      stream_id_ = any32(generator);
      sequence_ = any32(generator);
      timestamp_ = any32(generator);
      talkspurt_open_ = true;
      first_of_talkspurt = true;
      talkspurts_.fetch_add(1, std::memory_order_relaxed);
    }

    if (!sending) {
      if (talkspurt_open_) {
        // Closing the talkspurt: one last packet carrying TALKSPURT_END so the
        // receiver can drain rather than conceal its way to silence. Advisory,
        // and the receiver must not depend on it (spec section 3.1).
        radio::proto::MediaHeader header;
        header.stream_id = stream_id_;
        header.sequence = sequence_;
        header.timestamp = timestamp_;
        header.flags |= radio::proto::media_flag::kTalkspurtEnd;
        if (config_.fec) header.flags |= radio::proto::media_flag::kFec;

        const auto encoded = encoder_.encode(frame, payload);
        if (encoded.ok()) {
          const std::size_t length = radio::proto::encode_media(
              header, radio::ByteView{payload.data(), encoded.bytes}, datagram);
          if (length != 0) {
            (void)socket_.send_to(config_.peer,
                                  radio::ByteView{datagram.data(), length});
          }
        }
        ++sequence_;
        timestamp_ += radio::kFrameSamples;
        talkspurt_open_ = false;
      }
      continue;  // not transmitting: the frame is captured and discarded
    }

    const radio::Micros started = radio::now_us();
    const auto encoded = encoder_.encode(frame, payload);
    note_time(encode_calls_, encode_total_us_, encode_max_us_,
              static_cast<std::int64_t>(radio::now_us() - started));

    if (!encoded.ok()) {
      encode_failed_.fetch_add(1, std::memory_order_relaxed);
      continue;
    }

    radio::proto::MediaHeader header;
    header.stream_id = stream_id_;
    header.sequence = sequence_;
    header.timestamp = timestamp_;
    if (first_of_talkspurt) {
      header.flags |= radio::proto::media_flag::kTalkspurtStart;
    }
    if (config_.fec) header.flags |= radio::proto::media_flag::kFec;

    const std::size_t length = radio::proto::encode_media(
        header, radio::ByteView{payload.data(), encoded.bytes}, datagram);
    if (length == 0) {
      encode_failed_.fetch_add(1, std::memory_order_relaxed);
      continue;
    }

    const auto sent =
        socket_.send_to(config_.peer, radio::ByteView{datagram.data(), length});
    if (sent.ok()) {
      packets_sent_.fetch_add(1, std::memory_order_relaxed);
      bytes_sent_.fetch_add(static_cast<std::int64_t>(length),
                            std::memory_order_relaxed);
    } else {
      send_failed_.fetch_add(1, std::memory_order_relaxed);
      last_error_.store(sent.error);
    }

    ++sequence_;
    timestamp_ += radio::kFrameSamples;
  }
}

void Session::receive_available() noexcept {
  Datagram datagram;
  for (;;) {
    const auto received = socket_.recv_from(
        radio::ByteSpan{datagram.bytes.data(), datagram.bytes.size()});
    if (!received.ok()) return;

    datagram.length = static_cast<std::uint16_t>(received.bytes);
    datagram.arrival_us = received.arrival_us;
    datagrams_received_.fetch_add(1, std::memory_order_relaxed);

    // Handed over whole. The jitter buffer belongs to the playback callback and
    // nothing here may touch it.
    (void)rx_.push(datagram);
  }
}

void Session::network_loop() noexcept {
  // Ask for audio-adjacent scheduling.
  //
  // Measured before this was here: Opus encode took 750 us on an idle thread
  // and 5342 us mean inside a running session. now_us() is wall time, so the
  // difference was not codec cost -- it was this thread being descheduled
  // mid-encode while two audio callbacks and the UI ran. Every one of those
  // milliseconds is mouth-to-ear latency, so the fix is scheduling rather than
  // a faster encoder.
  //
  // gettid() through syscall() rather than the libc wrapper, which bionic only
  // declares from API 30; this app supports 26. A refusal is not fatal -- it
  // means slower, jitterier transmission, not a broken one -- so it is logged
  // and the loop carries on.
  const auto tid = static_cast<id_t>(syscall(SYS_gettid));
  if (setpriority(PRIO_PROCESS, tid, -16) != 0) {
    __android_log_print(ANDROID_LOG_WARN, kTag,
                        "could not raise network thread priority");
  }

  while (running_.load()) {
    // Blocks until a datagram arrives or the timeout expires, so an idle
    // session costs nothing and an arriving packet is picked up immediately.
    (void)socket_.wait_readable(kPollMs);
    receive_available();
    transmit_available();
  }
}

Session::Snapshot Session::snapshot() const noexcept {
  Snapshot out;
  out.running = running_.load();
  out.transmitting = transmitting_.load();

  out.packets_sent = packets_sent_.load();
  out.bytes_sent = bytes_sent_.load();
  out.send_failed = send_failed_.load();
  out.encode_failed = encode_failed_.load();
  out.talkspurts = talkspurts_.load();
  out.datagrams_received = datagrams_received_.load();
  out.rejected = rejected_.load();
  out.rx_ring_overflows = static_cast<std::int64_t>(rx_.overflows());
  out.tx_pcm_overflows = static_cast<std::int64_t>(tx_pcm_.overflows());

  const auto& queue = jitter_.queue();
  out.from_packet = static_cast<std::int64_t>(jitter_.from_packet());
  out.fec_recovered = static_cast<std::int64_t>(jitter_.fec_recovered());
  out.concealed = static_cast<std::int64_t>(jitter_.concealed());
  out.silence = static_cast<std::int64_t>(jitter_.silence());
  out.late = static_cast<std::int64_t>(queue.late());
  out.gaps = static_cast<std::int64_t>(queue.gaps());
  out.duplicates = static_cast<std::int64_t>(queue.duplicates());
  out.depth_frames = static_cast<std::int64_t>(queue.depth());

  out.encode_calls = encode_calls_.load();
  out.encode_total_us = encode_total_us_.load();
  out.encode_max_us = encode_max_us_.load();
  out.decode_calls = decode_calls_.load();
  out.decode_total_us = decode_total_us_.load();
  out.decode_max_us = decode_max_us_.load();
  return out;
}

}  // namespace fdradio
