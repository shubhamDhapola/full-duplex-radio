#include <array>
#include <cstdio>

#include "modes.hpp"
#include "radio/jitter_estimator.hpp"
#include "radio/proto.hpp"
#include "radio/seq_tracker.hpp"
#include "radio/udp_socket.hpp"
#include "util.hpp"

namespace radiobench {
namespace {

using namespace radio;

// Per-stream accounting, keyed on (peer, stream_id) exactly as spec section 3.2
// requires. A fixed table because the packet path must not allocate; eight
// slots is far more than M0 needs and M3 replaces this with the real peer
// table.
constexpr std::size_t kMaxStreams = 8;

struct StreamState {
  net::Endpoint peer{};
  std::uint32_t stream_id = 0;
  SeqTracker sequence{};
  JitterEstimator jitter{};
  Micros first_seen_us = 0;
  Micros last_seen_us = 0;
  std::uint64_t bytes = 0;
  bool active = false;
};

struct RejectCounts {
  std::array<std::uint64_t, 6> by_class{};

  void add(proto::Reject reject) {
    by_class[static_cast<std::size_t>(reject)] += 1;
  }
  [[nodiscard]] std::uint64_t total() const {
    std::uint64_t sum = 0;
    // Index 0 is Reject::None and is never counted.
    for (std::size_t i = 1; i < by_class.size(); ++i) sum += by_class[i];
    return sum;
  }
};

StreamState* find_or_open(std::array<StreamState, kMaxStreams>& table,
                          const net::Endpoint& peer, std::uint32_t stream_id,
                          Micros now) {
  for (auto& slot : table) {
    if (slot.active && slot.stream_id == stream_id && slot.peer == peer) {
      return &slot;
    }
  }
  for (auto& slot : table) {
    if (!slot.active) {
      slot = StreamState{};
      slot.peer = peer;
      slot.stream_id = stream_id;
      slot.first_seen_us = now;
      slot.active = true;
      return &slot;
    }
  }
  return nullptr;  // table full; caller counts the drop
}

void report_human(const net::UdpSocket& socket,
                  const std::array<StreamState, kMaxStreams>& table,
                  std::uint64_t pings, std::uint64_t pong_failed,
                  const RejectCounts& rejects, std::uint64_t unhandled,
                  std::uint64_t table_full) {
  const auto& stats = socket.stats();
  std::printf("\nradiobench respond on %s\n",
              socket.local_endpoint().to_string().c_str());
  std::printf("  datagrams      %llu received, %llu sent\n",
              static_cast<unsigned long long>(stats.datagrams_received),
              static_cast<unsigned long long>(stats.datagrams_sent));
  std::printf("  bytes          %llu received, %llu sent\n",
              static_cast<unsigned long long>(stats.bytes_received),
              static_cast<unsigned long long>(stats.bytes_sent));
  std::printf("  pings answered %llu\n",
              static_cast<unsigned long long>(pings));
  if (pong_failed != 0) {
    std::printf("  PONG FAILURES  %llu  (peer will see these as timeouts)\n",
                static_cast<unsigned long long>(pong_failed));
  }

  if (stats.truncated != 0) {
    std::printf("  TRUNCATED      %llu  (a peer exceeded the 1200-byte cap)\n",
                static_cast<unsigned long long>(stats.truncated));
  }
  if (rejects.total() != 0) {
    std::printf("  rejected       %llu total\n",
                static_cast<unsigned long long>(rejects.total()));
    for (std::size_t i = 1; i < rejects.by_class.size(); ++i) {
      if (rejects.by_class[i] == 0) continue;
      std::printf("    %-14s %llu\n",
                  proto::to_string(static_cast<proto::Reject>(i)),
                  static_cast<unsigned long long>(rejects.by_class[i]));
    }
  }
  if (unhandled != 0) {
    std::printf("  unhandled type %llu  (valid but not implemented in M0)\n",
                static_cast<unsigned long long>(unhandled));
  }
  if (table_full != 0) {
    std::printf("  streams capped %llu packets dropped, table full\n",
                static_cast<unsigned long long>(table_full));
  }

  for (const auto& stream : table) {
    if (!stream.active) continue;
    const auto& sequence = stream.sequence;
    const double seconds =
        static_cast<double>(stream.last_seen_us - stream.first_seen_us) / 1e6;

    std::printf("\n  stream 0x%08x from %s\n", stream.stream_id,
                stream.peer.to_string().c_str());
    std::printf("    received     %llu of %llu expected\n",
                static_cast<unsigned long long>(sequence.received()),
                static_cast<unsigned long long>(sequence.expected()));
    std::printf("    lost         %llu  (%.3f%%)\n",
                static_cast<unsigned long long>(sequence.lost()),
                sequence.loss_fraction() * 100.0);
    std::printf("    duplicates   %llu\n",
                static_cast<unsigned long long>(sequence.duplicates()));
    std::printf("    reordered    %llu\n",
                static_cast<unsigned long long>(sequence.reordered()));
    std::printf("    too old      %llu\n",
                static_cast<unsigned long long>(sequence.too_old()));
    std::printf(
        "    jitter       %.3f ms mean, %.3f ms peak\n",
        stream.jitter.jitter_ms(),
        static_cast<double>(stream.jitter.peak_abs_delta_samples()) / 48.0);
    if (seconds > 0.0) {
      std::printf("    bitrate      %.1f kbps over %.2f s\n",
                  static_cast<double>(stream.bytes) * 8.0 / seconds / 1000.0,
                  seconds);
    }
  }
  std::printf("\n");
}

void report_json(const net::UdpSocket& socket,
                 const std::array<StreamState, kMaxStreams>& table,
                 std::uint64_t pings, std::uint64_t pong_failed,
                 const RejectCounts& rejects, std::uint64_t unhandled) {
  const auto& stats = socket.stats();
  std::printf(
      R"({"mode":"respond","local":"%s","datagrams_received":%llu,)"
      R"("datagrams_sent":%llu,"bytes_received":%llu,"pings_answered":%llu,)"
      R"("pong_failed":%llu,"truncated":%llu,"rejected":%llu,)"
      R"("unhandled":%llu,"streams":[)",
      socket.local_endpoint().to_string().c_str(),
      static_cast<unsigned long long>(stats.datagrams_received),
      static_cast<unsigned long long>(stats.datagrams_sent),
      static_cast<unsigned long long>(stats.bytes_received),
      static_cast<unsigned long long>(pings),
      static_cast<unsigned long long>(pong_failed),
      static_cast<unsigned long long>(stats.truncated),
      static_cast<unsigned long long>(rejects.total()),
      static_cast<unsigned long long>(unhandled));

  bool first = true;
  for (const auto& stream : table) {
    if (!stream.active) continue;
    if (!first) std::printf(",");
    first = false;
    std::printf(
        R"({"stream_id":%u,"peer":"%s","received":%llu,"expected":%llu,)"
        R"("lost":%llu,"loss_fraction":%.6f,"duplicates":%llu,)"
        R"("reordered":%llu,"too_old":%llu,"jitter_ms":%.4f,"jitter_peak_ms":%.4f})",
        stream.stream_id, stream.peer.to_string().c_str(),
        static_cast<unsigned long long>(stream.sequence.received()),
        static_cast<unsigned long long>(stream.sequence.expected()),
        static_cast<unsigned long long>(stream.sequence.lost()),
        stream.sequence.loss_fraction(),
        static_cast<unsigned long long>(stream.sequence.duplicates()),
        static_cast<unsigned long long>(stream.sequence.reordered()),
        static_cast<unsigned long long>(stream.sequence.too_old()),
        stream.jitter.jitter_ms(),
        static_cast<double>(stream.jitter.peak_abs_delta_samples()) / 48.0);
  }
  std::printf("]}\n");
}

}  // namespace

int run_respond(const Options& options) {
  net::UdpSocket socket;
  net::UdpSocket::Options socket_options;
  socket_options.family = options.family;
  socket_options.port = options.port;

  if (!socket.open(socket_options)) {
    std::fprintf(stderr, "radiobench: could not bind port %u: %s\n",
                 static_cast<unsigned>(options.port),
                 std::strerror(socket.last_error()));
    return 1;
  }

  std::fprintf(stderr,
               "radiobench respond: listening on %s (Ctrl-C to report)\n",
               socket.local_endpoint().to_string().c_str());

  std::array<StreamState, kMaxStreams> streams{};
  RejectCounts rejects;
  std::uint64_t pings = 0;
  std::uint64_t pong_failed = 0;
  std::uint64_t unhandled = 0;
  std::uint64_t table_full = 0;

  std::array<std::byte, proto::kMaxDatagram> incoming{};
  std::array<std::byte, proto::kMaxDatagram> outgoing{};
  std::array<std::byte, proto::kPongBodySize> pong_body{};

  while (!stop_requested()) {
    // A 100 ms poll rather than an indefinite wait, so Ctrl-C is noticed
    // promptly without needing a self-pipe to interrupt the syscall.
    if (!socket.wait_readable(100)) continue;

    // Drain everything readable before returning to poll(). Under a burst one
    // poll can cover dozens of datagrams, and polling per packet would double
    // the syscall count for no benefit.
    for (;;) {
      const auto received = socket.recv_from(incoming);
      if (!received.ok()) break;  // EAGAIN: drained

      const ByteView datagram{incoming.data(), received.bytes};
      const auto type = proto::peek_type(datagram);
      if (!type) {
        rejects.add(type.reject);
        continue;
      }

      if (type.value == proto::Type::Ping) {
        const auto packet = proto::parse_control(datagram);
        if (!packet) {
          rejects.add(packet.reject);
          continue;
        }

        proto::PongBody body;
        body.orig_t1 = packet.value.header.send_time_us;
        body.recv_t2 = received.arrival_us;
        if (proto::encode_pong_body(body, pong_body) == 0) {
          ++pong_failed;
          continue;
        }

        proto::ControlHeader header;
        header.type = proto::Type::Pong;
        header.request_id = packet.value.header.request_id;
        // t3, taken as late as possible. The gap between recv_t2 and this is
        // our own processing time, and the RTT formula subtracts it out -- so a
        // slow responder does not inflate the peer's measurement of the
        // network.
        header.send_time_us = now_us();

        const std::size_t length =
            proto::encode_control(header, pong_body, outgoing);
        if (length == 0) {
          ++pong_failed;
          continue;
        }

        // A dropped PONG makes the peer record a timeout, so it must be counted
        // here or the loss looks like the peer's problem rather than ours.
        if (socket.send_to(received.from, ByteView{outgoing.data(), length})
                .ok()) {
          ++pings;
        } else {
          ++pong_failed;
        }
        continue;
      }

      if (type.value == proto::Type::Audio) {
        const auto packet = proto::parse_media(datagram);
        if (!packet) {
          rejects.add(packet.reject);
          continue;
        }

        const auto& header = packet.value.header;
        StreamState* stream = find_or_open(
            streams, received.from, header.stream_id, received.arrival_us);
        if (stream == nullptr) {
          ++table_full;
          continue;
        }

        stream->sequence.observe(header.sequence);
        // A talkspurt boundary re-anchors instead of contributing a sample: the
        // timestamp jump across a silence reflects how long the speaker paused,
        // not anything the network did. See JitterEstimator::reanchor.
        if (header.talkspurt_start()) {
          stream->jitter.reanchor(header.timestamp, received.arrival_us);
        } else {
          stream->jitter.observe(header.timestamp, received.arrival_us);
        }
        stream->last_seen_us = received.arrival_us;
        stream->bytes += received.bytes;
        continue;
      }

      ++unhandled;
    }
  }

  if (options.json) {
    report_json(socket, streams, pings, pong_failed, rejects, unhandled);
  } else {
    report_human(socket, streams, pings, pong_failed, rejects, unhandled,
                 table_full);
  }
  return 0;
}

}  // namespace radiobench
