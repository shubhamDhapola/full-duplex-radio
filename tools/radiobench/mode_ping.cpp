#include <array>
#include <cstdio>
#include <cstring>

#include "modes.hpp"
#include "radio/clock_sync.hpp"
#include "radio/histogram.hpp"
#include "radio/pacer.hpp"
#include "radio/proto.hpp"
#include "radio/udp_socket.hpp"
#include "util.hpp"

namespace radiobench {
namespace {

using namespace radio;

struct PingStats {
  std::uint64_t sent = 0;
  std::uint64_t replied = 0;
  std::uint64_t timed_out = 0;
  std::uint64_t send_failed = 0;
  // A PONG whose request id matches no outstanding probe. Usually a late reply
  // to a probe we already gave up on.
  std::uint64_t stale = 0;
  // A PONG that echoed an orig_t1 we never sent. The responder is buggy, or
  // something is rewriting packets.
  std::uint64_t bad_echo = 0;
  std::uint64_t malformed = 0;
};

double ms(std::int64_t microseconds) {
  return static_cast<double>(microseconds) / 1'000.0;
}

void report_human(const Options& options, const PingStats& stats,
                  const ClockSync& sync, const Histogram& request_latency) {
  const auto rtt = sync.rtt_histogram().snapshot();

  std::printf("\nradiobench ping -> %s\n", options.peer.to_string().c_str());
  std::printf("  probes         %llu sent, %llu replied, %llu timed out",
              static_cast<unsigned long long>(stats.sent),
              static_cast<unsigned long long>(stats.replied),
              static_cast<unsigned long long>(stats.timed_out));
  if (stats.sent != 0) {
    const double loss = static_cast<double>(stats.timed_out) /
                        static_cast<double>(stats.sent) * 100.0;
    std::printf("  (%.1f%% unanswered)", loss);
  }
  std::printf("\n");

  if (stats.replied == 0) {
    std::printf("\n  no replies -- is `radiobench respond` running on %s?\n\n",
                options.peer.to_string().c_str());
    return;
  }

  std::printf(
      "  rtt            p50 %.3f  p95 %.3f  p99 %.3f  min %.3f  max %.3f ms\n",
      ms(static_cast<std::int64_t>(rtt.p50)),
      ms(static_cast<std::int64_t>(rtt.p95)),
      ms(static_cast<std::int64_t>(rtt.p99)),
      ms(static_cast<std::int64_t>(rtt.min)),
      ms(static_cast<std::int64_t>(rtt.max)));

  // Kept separate from RTT on purpose. RTT subtracts the responder's processing
  // time; this does not. Conflating the two is how "ping is 3 ms" becomes a
  // claim about audio latency that the numbers do not support.
  const auto request = request_latency.snapshot();
  std::printf("  request        p50 %.3f  p99 %.3f ms  (includes peer processing)\n",
              ms(static_cast<std::int64_t>(request.p50)),
              ms(static_cast<std::int64_t>(request.p99)));

  const auto best = sync.best();
  std::printf("  clock offset   %+.3f ms +/- %.3f ms  (peer minus local)\n",
              ms(best.offset_us), ms(sync.offset_uncertainty_us()));
  std::printf("  selected from  rtt %.3f ms, the lowest of %llu accepted probes\n",
              ms(best.rtt_us), static_cast<unsigned long long>(sync.accepted()));

  if (sync.has_drift_estimate()) {
    const double ppm = sync.drift_ppm();
    std::printf("  clock drift    %+.2f ppm  (%.1f ms per hour of call)\n", ppm,
                ppm * 3'600.0 / 1'000.0);
  } else {
    std::printf("  clock drift    baseline too short -- needs >= 10 s of probing\n");
  }

  if (sync.rejected() != 0) {
    std::printf("  REJECTED       %llu impossible exchanges -- check PONG matching\n",
                static_cast<unsigned long long>(sync.rejected()));
  }
  if (stats.bad_echo != 0) {
    std::printf("  BAD ECHO       %llu replies echoed a t1 we never sent\n",
                static_cast<unsigned long long>(stats.bad_echo));
  }
  if (stats.stale != 0) {
    std::printf("  stale replies  %llu (arrived after their probe timed out)\n",
                static_cast<unsigned long long>(stats.stale));
  }
  std::printf(
      "\n  Note: one-way latency is not rtt/2. The offset above assumes a\n"
      "  symmetric path, so any one-way figure carries +/- %.3f ms of error.\n\n",
      ms(sync.offset_uncertainty_us()));
}

void report_json(const Options& options, const PingStats& stats,
                 const ClockSync& sync, const Histogram& request_latency) {
  const auto rtt = sync.rtt_histogram().snapshot();
  const auto request = request_latency.snapshot();
  const auto best = sync.best();

  // Emitted as a JSON number when known and as null when the baseline is too
  // short. A sentinel like 0 would be indistinguishable from "measured, and it
  // is zero", which is a genuinely different statement.
  char drift[32];
  if (sync.has_drift_estimate()) {
    std::snprintf(drift, sizeof(drift), "%.4f", sync.drift_ppm());
  } else {
    std::snprintf(drift, sizeof(drift), "null");
  }

  std::printf(
      R"({"mode":"ping","peer":"%s","sent":%llu,"replied":%llu,)"
      R"("timed_out":%llu,"stale":%llu,"bad_echo":%llu,"rejected":%llu,)"
      R"("rtt_us":{"min":%llu,"p50":%llu,"p95":%llu,"p99":%llu,"max":%llu},)"
      R"("request_us":{"p50":%llu,"p99":%llu},)"
      R"("offset_us":%lld,"offset_uncertainty_us":%lld,"selected_rtt_us":%lld,)"
      R"("drift_ppm":%s})"
      "\n",
      options.peer.to_string().c_str(),
      static_cast<unsigned long long>(stats.sent),
      static_cast<unsigned long long>(stats.replied),
      static_cast<unsigned long long>(stats.timed_out),
      static_cast<unsigned long long>(stats.stale),
      static_cast<unsigned long long>(stats.bad_echo),
      static_cast<unsigned long long>(sync.rejected()),
      static_cast<unsigned long long>(rtt.min),
      static_cast<unsigned long long>(rtt.p50),
      static_cast<unsigned long long>(rtt.p95),
      static_cast<unsigned long long>(rtt.p99),
      static_cast<unsigned long long>(rtt.max),
      static_cast<unsigned long long>(request.p50),
      static_cast<unsigned long long>(request.p99),
      static_cast<long long>(best.offset_us),
      static_cast<long long>(sync.offset_uncertainty_us()),
      static_cast<long long>(best.rtt_us),
      drift);
}

}  // namespace

int run_ping(const Options& options) {
  net::UdpSocket socket;
  net::UdpSocket::Options socket_options;
  socket_options.family = options.family;
  socket_options.port = 0;  // ephemeral: a client needs no fixed port

  if (!socket.open(socket_options)) {
    std::fprintf(stderr, "radiobench: could not open socket: %s\n",
                 std::strerror(socket.last_error()));
    return 1;
  }

  ClockSync sync;
  Histogram request_latency;
  PingStats stats;

  // The probe interval is paced from a fixed schedule, so oversleep on one probe
  // does not shift the rest -- the same reason the media path uses a pacer.
  Pacer pacer(static_cast<Micros>(options.interval_ms) * 1'000ull);

  std::array<std::byte, proto::kMaxDatagram> outgoing{};
  std::array<std::byte, proto::kMaxDatagram> incoming{};

  std::fprintf(stderr, "radiobench ping: %u probes to %s every %u ms\n",
               options.count, options.peer.to_string().c_str(),
               options.interval_ms);

  pacer.start(now_us());

  for (std::uint32_t probe = 0; probe < options.count && !stop_requested();
       ++probe) {
    sleep_us(pacer.delay_until_due_us(now_us()));

    const std::uint32_t request_id = probe + 1;

    proto::ControlHeader header;
    header.type = proto::Type::Ping;
    header.request_id = request_id;
    const Micros t1 = now_us();
    header.send_time_us = t1;

    const std::size_t length = proto::encode_control(header, ByteView{}, outgoing);
    const auto sent = socket.send_to(options.peer, ByteView{outgoing.data(), length});
    pacer.advance(now_us());

    if (!sent.ok()) {
      ++stats.send_failed;
      std::fprintf(stderr, "radiobench: send failed: %s\n",
                   std::strerror(sent.error));
      continue;
    }
    ++stats.sent;

    const Micros deadline = t1 + static_cast<Micros>(options.timeout_ms) * 1'000ull;
    bool matched = false;

    while (!matched && !stop_requested()) {
      const Micros now = now_us();
      if (now >= deadline) break;

      const auto remaining_ms = static_cast<int>((deadline - now) / 1'000ull) + 1;
      if (!socket.wait_readable(remaining_ms)) continue;

      const auto received = socket.recv_from(incoming);
      if (!received.ok()) continue;

      // t4 comes from the socket, stamped immediately after recvmsg returned.
      const Micros t4 = received.arrival_us;

      const auto packet =
          proto::parse_control(ByteView{incoming.data(), received.bytes});
      if (!packet || packet.value.header.type != proto::Type::Pong) {
        ++stats.malformed;
        continue;
      }
      if (packet.value.header.request_id != request_id) {
        // A reply to an earlier probe we already abandoned. Discard rather than
        // pair it with this probe's t1, which would fabricate an RTT.
        ++stats.stale;
        continue;
      }

      const auto body = proto::parse_pong_body(packet.value.body);
      if (!body) {
        ++stats.malformed;
        continue;
      }

      // The responder echoes t1, but our local value is the authoritative one:
      // trusting the echo would let a buggy or hostile peer choose our
      // measurement. The echo is checked as a peer sanity test instead.
      if (body.value.orig_t1 != t1) ++stats.bad_echo;

      const Micros t2 = body.value.recv_t2;
      const Micros t3 = packet.value.header.send_time_us;

      if (sync.observe(t1, t2, t3, t4).valid) {
        ++stats.replied;
        request_latency.record(t4 - t1);
        matched = true;
      } else {
        // Counted inside ClockSync::rejected().
        matched = true;
      }
    }

    if (!matched) ++stats.timed_out;
  }

  if (options.json) {
    report_json(options, stats, sync, request_latency);
  } else {
    report_human(options, stats, sync, request_latency);
  }
  return stats.replied > 0 ? 0 : 1;
}

}  // namespace radiobench
