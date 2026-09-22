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

  // Prober and receive-path accounting. Reset before the thread is started, so
  // the thread never races the reset.
  prober_.reset();
  rx_seq_.reset();
  rx_jitter_.reset();
  rx_stream_open_ = false;

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

  link_.store(static_cast<std::int32_t>(Link::Probing));
  probes_sent_.store(0);
  pongs_received_.store(0);
  probes_timed_out_.store(0);
  probes_stale_.store(0);
  probes_bad_echo_.store(0);
  probes_impossible_.store(0);
  pongs_sent_.store(0);
  rtt_last_us_.store(0);
  rtt_best_us_.store(0);
  offset_us_.store(0);
  offset_uncertainty_us_.store(0);
  rx_jitter_us_.store(0);
  rx_received_.store(0);
  rx_lost_.store(0);
  rx_duplicates_.store(0);
  rx_reordered_.store(0);
  unhandled_.store(0);

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
  link_.store(static_cast<std::int32_t>(Link::Idle));
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
    // Already parsed, validated and accounted for on the network thread. All
    // that is left is to point the payload view at this ring slot, which is
    // where the bytes now live.
    radio::proto::MediaPacket packet;
    packet.header = datagram.header;
    packet.payload =
        radio::ByteView{datagram.bytes.data() + datagram.payload_offset,
                        datagram.payload_length};
    (void)jitter_.push(packet, datagram.arrival_us);
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

        // Counted like any other packet, because it is one. Leaving it out
        // made the screen report 300 sent where `radiobench respond` counted
        // 301 received of 301 expected -- a discrepancy that reads as the
        // receiver inventing a packet, when in fact the sender was not
        // counting its own.
        const radio::Micros started = radio::now_us();
        const auto encoded = encoder_.encode(frame, payload);
        note_time(encode_calls_, encode_total_us_, encode_max_us_,
                  static_cast<std::int64_t>(radio::now_us() - started));

        if (encoded.ok()) {
          const std::size_t length = radio::proto::encode_media(
              header, radio::ByteView{payload.data(), encoded.bytes}, datagram);
          if (length != 0) {
            const auto sent = socket_.send_to(
                config_.peer, radio::ByteView{datagram.data(), length});
            if (sent.ok()) {
              packets_sent_.fetch_add(1, std::memory_order_relaxed);
              bytes_sent_.fetch_add(static_cast<std::int64_t>(length),
                                    std::memory_order_relaxed);
            } else {
              send_failed_.fetch_add(1, std::memory_order_relaxed);
              last_error_.store(sent.error);
            }
          } else {
            encode_failed_.fetch_add(1, std::memory_order_relaxed);
          }
        } else {
          encode_failed_.fetch_add(1, std::memory_order_relaxed);
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

void Session::observe_media(const radio::proto::MediaHeader& header,
                            radio::Micros arrival_us) noexcept {
  // A talkspurt is a stream, so a new stream id means the previous speaker's
  // accounting is finished and a fresh one begins. Carrying the old sequence
  // window across would report the whole distance between two random starting
  // sequences as loss.
  if (!rx_stream_open_ || header.stream_id != rx_stream_id_) {
    rx_seq_.reset();
    rx_jitter_.reset();
    rx_stream_id_ = header.stream_id;
    rx_stream_open_ = true;
  }

  rx_seq_.observe(header.sequence);

  // A talkspurt boundary re-anchors instead of contributing a sample: the
  // timestamp jump across a silence measures how long the speaker paused, not
  // anything the network did. Same rule `radiobench respond` applies, and the
  // reason RFC 3550 jitter needs it is in jitter_estimator.hpp.
  if (header.talkspurt_start()) {
    rx_jitter_.reanchor(header.timestamp, arrival_us);
  } else {
    rx_jitter_.observe(header.timestamp, arrival_us);
  }

  rx_received_.store(static_cast<std::int64_t>(rx_seq_.received()),
                     std::memory_order_relaxed);
  rx_lost_.store(static_cast<std::int64_t>(rx_seq_.lost()),
                 std::memory_order_relaxed);
  rx_duplicates_.store(static_cast<std::int64_t>(rx_seq_.duplicates()),
                       std::memory_order_relaxed);
  rx_reordered_.store(static_cast<std::int64_t>(rx_seq_.reordered()),
                      std::memory_order_relaxed);
  rx_jitter_us_.store(static_cast<std::int64_t>(rx_jitter_.jitter_us()),
                      std::memory_order_relaxed);
}

void Session::receive_available() noexcept {
  // WHY THE NETWORK THREAD PARSES AND MEASURES, AND THE AUDIO CALLBACK DOES NOT
  //
  // Jitter, loss and rejection are facts about the *path*, and the honest place
  // to measure them is where the datagram arrived -- next to the arrival
  // timestamp the kernel handed us. Measuring them at playout instead makes
  // them conditional on playout still running, so the moment the audio callback
  // stalls the counters freeze at exactly the moment you most want to read
  // them. Doing it here also makes the callback lighter, not heavier.
  //
  // The split that remains is the one that matters: the jitter buffer is
  // single-threaded by contract, and nothing in this function touches it.
  Datagram datagram;
  for (;;) {
    const auto received = socket_.recv_from(
        radio::ByteSpan{datagram.bytes.data(), datagram.bytes.size()});
    if (!received.ok()) return;

    datagram.arrival_us = received.arrival_us;
    datagrams_received_.fetch_add(1, std::memory_order_relaxed);

    const radio::ByteView view{datagram.bytes.data(), received.bytes};

    // Classify on four bytes before anything else looks at the datagram. A
    // malformed packet is then rejected by code that has touched the common
    // prefix and no stream state (spec section 6.1, and peek_type in
    // proto.hpp).
    const auto type = radio::proto::peek_type(view);
    if (!type) {
      rejected_.fetch_add(1, std::memory_order_relaxed);
      continue;
    }

    if (type.value == radio::proto::Type::Pong) {
      const auto packet = radio::proto::parse_control(view);
      if (!packet) {
        rejected_.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      prober_.on_pong(packet.value, received.arrival_us);
      continue;
    }

    if (type.value == radio::proto::Type::Ping) {
      const auto packet = radio::proto::parse_control(view);
      if (!packet) {
        rejected_.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      // Answered from whoever asked, not from config_.peer: a peer probing us
      // has not necessarily been configured as ours, and this is what lets
      // `radiobench ping` on the workstation measure the phone.
      answer_ping(packet.value, received.from, received.arrival_us);
      continue;
    }

    if (type.value != radio::proto::Type::Audio) {
      // An assigned type we have not implemented yet -- JOIN and friends,
      // reserved for M3. Counted separately from a rejection because a peer
      // running ahead of us is a different situation from a broken packet.
      unhandled_.fetch_add(1, std::memory_order_relaxed);
      continue;
    }

    const auto packet = radio::proto::parse_media(view);
    if (!packet) {
      rejected_.fetch_add(1, std::memory_order_relaxed);
      continue;
    }

    observe_media(packet.value.header, received.arrival_us);

    // The payload offset is derived from where the parser put the view, not
    // assumed to be sizeof(header): if the wire format ever grows an extension,
    // this keeps working and a hardcoded 16 would silently ship garbage.
    datagram.header = packet.value.header;
    datagram.payload_offset = static_cast<std::uint16_t>(
        packet.value.payload.data() - datagram.bytes.data());
    datagram.payload_length =
        static_cast<std::uint16_t>(packet.value.payload.size());

    // Handed over whole. The jitter buffer belongs to the playback callback and
    // nothing here may touch it.
    (void)rx_.push(datagram);
  }
}

void Session::probe_if_due(radio::Micros now) noexcept {
  std::array<std::byte, radio::proto::kControlHeaderSize> outgoing{};
  const auto probe = prober_.due_probe(now, outgoing);
  if (!probe.ready()) return;

  if (!socket_
           .send_to(config_.peer,
                    radio::ByteView{outgoing.data(), probe.length})
           .ok()) {
    // It never left, so it is withdrawn rather than left to time out. A probe
    // our own socket refused is not the peer failing to answer, and folding the
    // two together would make a local fault read as a dead link.
    prober_.withdraw(probe.request_id);
  }
}

void Session::answer_ping(const radio::proto::ControlPacket& packet,
                          const radio::net::Endpoint& from,
                          radio::Micros t2) noexcept {
  std::array<std::byte,
             radio::proto::kControlHeaderSize + radio::proto::kPongBodySize>
      outgoing{};
  // t3 read here, as late as it can be: the gap between t2 and it is our own
  // thinking time, and the prober on the other end subtracts it out.
  const std::size_t length =
      radio::encode_pong_for(packet, t2, radio::now_us(), outgoing);
  if (length == 0) return;

  if (socket_.send_to(from, radio::ByteView{outgoing.data(), length}).ok()) {
    pongs_sent_.fetch_add(1, std::memory_order_relaxed);
  }
}

void Session::publish_probe_stats(radio::Micros now) noexcept {
  const auto& stats = prober_.stats();
  probes_sent_.store(static_cast<std::int64_t>(stats.sent),
                     std::memory_order_relaxed);
  pongs_received_.store(static_cast<std::int64_t>(stats.replied),
                        std::memory_order_relaxed);
  probes_timed_out_.store(static_cast<std::int64_t>(stats.timed_out),
                          std::memory_order_relaxed);
  probes_stale_.store(static_cast<std::int64_t>(stats.stale),
                      std::memory_order_relaxed);
  probes_bad_echo_.store(static_cast<std::int64_t>(stats.bad_echo),
                         std::memory_order_relaxed);
  probes_impossible_.store(static_cast<std::int64_t>(stats.impossible),
                           std::memory_order_relaxed);

  const auto best = prober_.sync().best();
  rtt_last_us_.store(prober_.last_rtt_us(), std::memory_order_relaxed);
  rtt_best_us_.store(best.rtt_us, std::memory_order_relaxed);
  offset_us_.store(best.offset_us, std::memory_order_relaxed);
  offset_uncertainty_us_.store(prober_.sync().offset_uncertainty_us(),
                               std::memory_order_relaxed);

  const radio::Micros last = prober_.last_reply_us();
  Link state;
  if (last == 0) {
    state = Link::Probing;
  } else if (now - last <= kLinkTimeoutUs) {
    state = Link::Up;
  } else {
    state = Link::Lost;
  }
  link_.store(static_cast<std::int32_t>(state), std::memory_order_relaxed);
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

  radio::Prober::Config probe_config;
  probe_config.interval_us =
      static_cast<radio::Micros>(config_.probe_interval_ms) * 1'000ull;
  probe_config.timeout_us =
      static_cast<radio::Micros>(config_.probe_timeout_ms) * 1'000ull;
  prober_.start(probe_config, radio::now_us());

  while (running_.load()) {
    // Blocks until a datagram arrives or the timeout expires, so an idle
    // session costs nothing and an arriving packet is picked up immediately.
    (void)socket_.wait_readable(kPollMs);
    receive_available();
    transmit_available();

    // WHY THE PROBER IS ASYNCHRONOUS AND `radiobench ping` IS NOT
    //
    // The host tool sends a probe and then sits in a receive loop until the
    // PONG comes back or the deadline passes, because measuring is all it is
    // doing. This thread cannot: it is also draining the microphone and
    // servicing media, and blocking for up to a second would stall both.
    //
    // So the probe is fired on a schedule, the outstanding ones live in a small
    // table, and replies are matched whenever they turn up in the ordinary
    // receive path. It is the same measurement; only the waiting is different.
    const radio::Micros now = radio::now_us();
    probe_if_due(now);
    prober_.expire(now);
    publish_probe_stats(now);
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

  out.link = link_.load();
  out.probes_sent = probes_sent_.load();
  out.pongs_received = pongs_received_.load();
  out.probes_timed_out = probes_timed_out_.load();
  out.probes_stale = probes_stale_.load();
  out.probes_bad_echo = probes_bad_echo_.load();
  out.probes_impossible = probes_impossible_.load();
  out.pongs_sent = pongs_sent_.load();
  out.rtt_last_us = rtt_last_us_.load();
  out.rtt_best_us = rtt_best_us_.load();
  out.offset_us = offset_us_.load();
  out.offset_uncertainty_us = offset_uncertainty_us_.load();

  out.rx_jitter_us = rx_jitter_us_.load();
  out.rx_received = rx_received_.load();
  out.rx_lost = rx_lost_.load();
  out.rx_duplicates = rx_duplicates_.load();
  out.rx_reordered = rx_reordered_.load();
  out.unhandled = unhandled_.load();
  return out;
}

}  // namespace fdradio
