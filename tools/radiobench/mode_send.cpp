#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "modes.hpp"
#include "radio/pacer.hpp"
#include "radio/proto.hpp"
#include "radio/trace.hpp"
#include "radio/udp_socket.hpp"
#include "util.hpp"

namespace radiobench {
namespace {

using namespace radio;

struct SendStats {
  std::uint64_t attempted = 0;
  std::uint64_t sent = 0;
  std::uint64_t failed = 0;
  std::uint64_t encode_failed = 0;
  Micros started_us = 0;
  Micros finished_us = 0;
};

void report_human(const Options& options, const SendStats& stats,
                  const Pacer& pacer, const net::UdpSocket& socket,
                  const TraceBuffer& trace, std::uint32_t stream_id) {
  const double seconds =
      static_cast<double>(stats.finished_us - stats.started_us) / 1e6;
  const auto& counters = socket.stats();

  std::printf("\nradiobench send -> %s\n", options.peer.to_string().c_str());
  std::printf("  stream         0x%08x\n", stream_id);
  std::printf("  packets        %llu sent",
              static_cast<unsigned long long>(stats.sent));
  if (stats.failed != 0) {
    std::printf(", %llu failed", static_cast<unsigned long long>(stats.failed));
  }
  std::printf("\n");
  std::printf("  duration       %.3f s\n", seconds);
  if (seconds > 0.0) {
    std::printf("  actual rate    %.2f pps  (requested %u)\n",
                static_cast<double>(stats.sent) / seconds, options.rate_pps);
    std::printf(
        "  wire bitrate   %.1f kbps  (%u byte payload + 16 byte header)\n",
        static_cast<double>(counters.bytes_sent) * 8.0 / seconds / 1000.0,
        options.payload_bytes);
  }

  // A non-zero resync count means the send thread was starved for longer than
  // two frame intervals. That is a finding about scheduling, not a curiosity:
  // the frames due during the stall were never sent.
  if (pacer.resyncs() != 0) {
    std::printf(
        "  PACER RESYNCS  %llu  (send thread starved; frames dropped)\n",
        static_cast<unsigned long long>(pacer.resyncs()));
  } else {
    std::printf("  pacer          on schedule, no resyncs\n");
  }

  if (!options.trace_path.empty()) {
    std::printf("  trace          %zu records -> %s\n", trace.size(),
                options.trace_path.c_str());
    if (trace.overwritten() != 0) {
      std::printf(
          "  TRACE TRUNCATED %llu records overwritten; raise capacity\n",
          static_cast<unsigned long long>(trace.overwritten()));
    }
  }
  std::printf("\n");
}

void report_json(const Options& options, const SendStats& stats,
                 const Pacer& pacer, const net::UdpSocket& socket,
                 const TraceBuffer& trace, std::uint32_t stream_id) {
  const double seconds =
      static_cast<double>(stats.finished_us - stats.started_us) / 1e6;
  std::printf(
      R"({"mode":"send","peer":"%s","stream_id":%u,"sent":%llu,"failed":%llu,)"
      R"("duration_s":%.6f,"rate_pps":%.3f,"bytes_sent":%llu,)"
      R"("pacer_resyncs":%llu,"trace_records":%zu,"trace_overwritten":%llu})"
      "\n",
      options.peer.to_string().c_str(), stream_id,
      static_cast<unsigned long long>(stats.sent),
      static_cast<unsigned long long>(stats.failed), seconds,
      seconds > 0.0 ? static_cast<double>(stats.sent) / seconds : 0.0,
      static_cast<unsigned long long>(socket.stats().bytes_sent),
      static_cast<unsigned long long>(pacer.resyncs()), trace.size(),
      static_cast<unsigned long long>(trace.overwritten()));
}

}  // namespace

int run_send(const Options& options) {
  if (options.payload_bytes > proto::kMaxPayload) {
    std::fprintf(stderr,
                 "radiobench: payload %u exceeds the %zu byte protocol cap\n",
                 options.payload_bytes, proto::kMaxPayload);
    return 2;
  }

  net::UdpSocket socket;
  net::UdpSocket::Options socket_options;
  socket_options.family = options.family;
  socket_options.port = 0;

  if (!socket.open(socket_options)) {
    std::fprintf(stderr, "radiobench: could not open socket: %s\n",
                 std::strerror(socket.last_error()));
    return 1;
  }

  // Spec section 3.3 requires random starting values for stream id, sequence
  // and timestamp. Seeding from --seed makes a run byte-for-byte reproducible,
  // which is what lets an A/B comparison through the impairment proxy attribute
  // a difference to the change under test rather than to a different packet
  // stream.
  std::mt19937 generator{options.seed.has_value() ? *options.seed
                                                  : std::random_device{}()};
  std::uniform_int_distribution<std::uint32_t> any32{0, 0xFFFFFFFFu};
  const std::uint32_t stream_id = any32(generator);
  std::uint32_t sequence = any32(generator);
  std::uint32_t timestamp = any32(generator);

  // Payload is a recognisable pattern rather than zeros, so truncation or
  // corruption in a capture is visible by eye.
  std::vector<std::byte> payload(options.payload_bytes);
  for (std::size_t i = 0; i < payload.size(); ++i) {
    payload[i] = static_cast<std::byte>(0x40u + (i % 0x30u));
  }

  const auto interval_us = static_cast<Micros>(1'000'000ull / options.rate_pps);
  // Samples per frame follows the packet rate so the timestamp advances at the
  // media clock rate, exactly as a real encoder would drive it.
  const auto samples_per_frame = kSampleRateHz / options.rate_pps;

  Pacer pacer(interval_us);
  std::array<std::byte, proto::kMaxDatagram> outgoing{};

  const auto expected_packets =
      static_cast<std::size_t>(options.duration_s *
                               static_cast<double>(options.rate_pps)) +
      64;
  TraceBuffer trace(options.trace_path.empty() ? 1 : expected_packets);

  SendStats stats;
  stats.started_us = now_us();
  const Micros deadline =
      stats.started_us + static_cast<Micros>(options.duration_s * 1e6);

  std::fprintf(stderr,
               "radiobench send: stream 0x%08x to %s at %u pps for %.1f s\n",
               stream_id, options.peer.to_string().c_str(), options.rate_pps,
               options.duration_s);

  pacer.start(stats.started_us);
  bool first_packet = true;

  while (now_us() < deadline && !stop_requested()) {
    sleep_us(pacer.delay_until_due_us(now_us()));
    if (now_us() >= deadline || stop_requested()) break;

    proto::MediaHeader header;
    header.stream_id = stream_id;
    header.sequence = sequence;
    header.timestamp = timestamp;
    if (first_packet) {
      header.flags |= proto::media_flag::kTalkspurtStart;
      first_packet = false;
    }

    TraceRecord record;
    record.stream_id = stream_id;
    record.sequence = sequence;
    record.mark(Stage::Queued, now_us());

    const std::size_t length = proto::encode_media(header, payload, outgoing);
    if (length == 0) {
      ++stats.encode_failed;
      break;
    }

    ++stats.attempted;
    const auto sent =
        socket.send_to(options.peer, ByteView{outgoing.data(), length});
    record.mark(Stage::Sent, now_us());

    if (sent.ok()) {
      ++stats.sent;
    } else {
      ++stats.failed;
    }

    if (!options.trace_path.empty()) trace.record(record);

    pacer.advance(now_us());
    ++sequence;
    timestamp += samples_per_frame;
  }

  // Mark the end of the talkspurt so the receiver can release stream state
  // rather than waiting for a timeout. Advisory: it may be lost, and the
  // receiver must not depend on it.
  if (stats.sent != 0) {
    proto::MediaHeader header;
    header.stream_id = stream_id;
    header.sequence = sequence;
    header.timestamp = timestamp;
    header.flags |= proto::media_flag::kTalkspurtEnd;
    const std::size_t length = proto::encode_media(header, payload, outgoing);
    if (length != 0 &&
        !socket.send_to(options.peer, ByteView{outgoing.data(), length}).ok()) {
      // Advisory per spec section 3.1: the receiver releases stream state on a
      // timeout anyway. Reported rather than ignored so a systematic send
      // failure at end-of-stream is visible.
      std::fprintf(stderr,
                   "radiobench: talkspurt-end send failed (advisory)\n");
    }
  }

  stats.finished_us = now_us();

  if (!options.trace_path.empty()) {
    std::FILE* out = std::fopen(options.trace_path.c_str(), "w");
    if (out == nullptr) {
      std::fprintf(stderr, "radiobench: could not open trace file %s: %s\n",
                   options.trace_path.c_str(), std::strerror(errno));
    } else {
      // Written after the run, never during it: file I/O on the send path would
      // be a latency spike measured as network jitter.
      if (!trace.write_csv(out)) {
        std::fprintf(stderr, "radiobench: trace write failed\n");
      }
      std::fclose(out);
    }
  }

  if (options.json) {
    report_json(options, stats, pacer, socket, trace, stream_id);
  } else {
    report_human(options, stats, pacer, socket, trace, stream_id);
  }
  return stats.sent > 0 ? 0 : 1;
}

}  // namespace radiobench
