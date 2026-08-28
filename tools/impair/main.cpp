#include <array>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>

#include "radio/clock.hpp"
#include "radio/impairment.hpp"
#include "radio/proto.hpp"
#include "radio/udp_socket.hpp"
#include "scenario.hpp"

namespace {

using namespace radio;
using impair::Scenario;

volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

// A packet waiting for its release time.
//
// Storage is a fixed array of full-MTU slots, allocated once. The relay is the
// instrument, so it must not introduce latency of its own: an allocation here
// would show up in the measurements as network jitter that the model never
// asked for.
struct Pending {
  Micros release_us = 0;
  std::size_t length = 0;
  bool to_forward = false;  // true: send upstream; false: back to the client
  bool occupied = false;
  std::uint64_t serial = 0;
  std::array<std::byte, proto::kMaxDatagram> data{};
};

// Reordering is not implemented as a shuffle. Each packet gets its own release
// time, and a packet given more delay than its successor simply comes out
// second. That is how a real network reorders, and it means the delay
// distribution and the reordering rate cannot disagree with each other.
// 1024 full-MTU slots is 1.2 MB. At 50 pps even a two-second Pareto tail holds
// only ~100 packets, so this is 10x headroom -- and the smaller allocation
// matters because it happens before the proxy can receive anything, so it is
// dead time during which a sender's packets would be missed.
constexpr std::size_t kQueueCapacity = 1'024;

struct LogRow {
  std::uint64_t serial = 0;
  bool to_forward = false;
  sim::Verdict verdict = sim::Verdict::Forward;
  Micros arrival_us = 0;
  Micros release_us = 0;
  Micros delay_us = 0;
  std::size_t bytes = 0;
  const char* packet_type = "other";
  // Parsed out of AUDIO packets so the log names the exact sequence numbers it
  // dropped. That turns validation of radiobench's loss counter from
  // "the rates look similar" into an exact set comparison.
  bool has_media = false;
  std::uint32_t stream_id = 0;
  std::uint32_t sequence = 0;
  bool duplicate = false;
  bool reordered = false;
};

struct Direction {
  sim::ImpairmentEngine engine;
  std::uint64_t received = 0;
  std::uint64_t emitted = 0;
  std::uint64_t queue_full = 0;
  std::uint64_t send_errors = 0;
};

Pending* claim_slot(std::vector<Pending>& queue) {
  for (auto& slot : queue) {
    if (!slot.occupied) return &slot;
  }
  return nullptr;
}

void classify(ByteView datagram, LogRow& row) {
  const auto type = proto::peek_type(datagram);
  if (!type) {
    row.packet_type = "unparsed";
    return;
  }
  switch (type.value) {
    case proto::Type::Audio: {
      row.packet_type = "audio";
      const auto media = proto::parse_media(datagram);
      if (media) {
        row.has_media = true;
        row.stream_id = media.value.header.stream_id;
        row.sequence = media.value.header.sequence;
      }
      break;
    }
    case proto::Type::Ping:
      row.packet_type = "ping";
      break;
    case proto::Type::Pong:
      row.packet_type = "pong";
      break;
    default:
      row.packet_type = "control";
      break;
  }
}

void write_log(const std::string& path, const std::vector<LogRow>& rows) {
  std::FILE* out = std::fopen(path.c_str(), "w");
  if (out == nullptr) {
    std::fprintf(stderr, "impair: cannot write log '%s': %s\n", path.c_str(),
                 std::strerror(errno));
    return;
  }
  std::fprintf(out,
               "serial,direction,verdict,arrival_us,release_us,delay_us,bytes,"
               "type,stream_id,sequence,duplicate,reordered\n");
  for (const auto& row : rows) {
    std::fprintf(out, "%llu,%s,%s,%llu,%llu,%llu,%zu,%s,",
                 static_cast<unsigned long long>(row.serial),
                 row.to_forward ? "upstream" : "downstream",
                 sim::to_string(row.verdict),
                 static_cast<unsigned long long>(row.arrival_us),
                 static_cast<unsigned long long>(row.release_us),
                 static_cast<unsigned long long>(row.delay_us), row.bytes,
                 row.packet_type);
    if (row.has_media) {
      std::fprintf(out, "%u,%u,", row.stream_id, row.sequence);
    } else {
      std::fprintf(out, ",,");
    }
    std::fprintf(out, "%d,%d\n", row.duplicate ? 1 : 0, row.reordered ? 1 : 0);
  }
  std::fclose(out);
}

void report_human(const Scenario& scenario, const Direction& up,
                  const Direction& down, std::size_t log_rows,
                  std::uint64_t discarded_at_exit) {
  const auto show = [](const char* label, const Direction& d) {
    const auto& c = d.engine.counters();
    std::printf("  %-10s %llu in, %llu out\n", label,
                static_cast<unsigned long long>(d.received),
                static_cast<unsigned long long>(d.emitted));
    std::printf("             dropped %llu total  (%.3f%%)"
                "  independent %llu, burst %llu, rate %llu\n",
                static_cast<unsigned long long>(d.engine.dropped_total()),
                d.engine.measured_loss_fraction() * 100.0,
                static_cast<unsigned long long>(c.dropped_independent),
                static_cast<unsigned long long>(c.dropped_burst),
                static_cast<unsigned long long>(c.dropped_rate_limit));
    std::printf("             duplicated %llu, reordered %llu, bursts %llu\n",
                static_cast<unsigned long long>(c.duplicated),
                static_cast<unsigned long long>(c.reordered),
                static_cast<unsigned long long>(c.bursts_entered));
    if (d.queue_full != 0) {
      std::printf("             QUEUE FULL %llu dropped -- raise capacity\n",
                  static_cast<unsigned long long>(d.queue_full));
    }
    if (d.send_errors != 0) {
      std::printf("             send errors %llu\n",
                  static_cast<unsigned long long>(d.send_errors));
    }
  };

  std::printf("\nimpair '%s' seed %llu\n", scenario.name.c_str(),
              static_cast<unsigned long long>(scenario.seed));
  show("upstream", up);
  show("downstream", down);
  if (discarded_at_exit != 0) {
    std::printf("  DISCARDED  %llu packets still queued at exit\n",
                static_cast<unsigned long long>(discarded_at_exit));
  } else {
    std::printf("  drain      queue emptied cleanly, nothing discarded\n");
  }
  if (!scenario.log_path.empty()) {
    std::printf("  log        %zu events -> %s\n", log_rows,
                scenario.log_path.c_str());
  }
  std::printf("\n");
}

void report_json(const Scenario& scenario, const Direction& up,
                 const Direction& down, std::size_t log_rows,
                 std::uint64_t discarded_at_exit) {
  const auto emit = [](const char* label, const Direction& d, bool last) {
    const auto& c = d.engine.counters();
    std::printf(
        R"("%s":{"received":%llu,"emitted":%llu,"dropped":%llu,)"
        R"("loss_fraction":%.6f,"dropped_independent":%llu,)"
        R"("dropped_burst":%llu,"dropped_rate_limit":%llu,)"
        R"("duplicated":%llu,"reordered":%llu,"bursts":%llu,"queue_full":%llu}%s)",
        label, static_cast<unsigned long long>(d.received),
        static_cast<unsigned long long>(d.emitted),
        static_cast<unsigned long long>(d.engine.dropped_total()),
        d.engine.measured_loss_fraction(),
        static_cast<unsigned long long>(c.dropped_independent),
        static_cast<unsigned long long>(c.dropped_burst),
        static_cast<unsigned long long>(c.dropped_rate_limit),
        static_cast<unsigned long long>(c.duplicated),
        static_cast<unsigned long long>(c.reordered),
        static_cast<unsigned long long>(c.bursts_entered),
        static_cast<unsigned long long>(d.queue_full), last ? "" : ",");
  };

  std::printf(R"({"tool":"impair","scenario":"%s","seed":%llu,)",
              scenario.name.c_str(),
              static_cast<unsigned long long>(scenario.seed));
  emit("upstream", up, false);
  emit("downstream", down, false);
  std::printf(R"("log_events":%zu,"discarded_at_exit":%llu})" "\n", log_rows,
              static_cast<unsigned long long>(discarded_at_exit));
}

}  // namespace

int main(int argc, char** argv) {
  const auto parsed = impair::parse_arguments(argc, argv);
  if (!parsed) {
    impair::print_usage(stderr);
    return 2;
  }
  if (parsed->help) {
    impair::print_usage(stdout);
    return 0;
  }
  const Scenario scenario = *parsed;

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  net::UdpSocket client_side;
  net::UdpSocket forward_side;

  net::UdpSocket::Options client_options;
  client_options.family = scenario.listen.family();
  client_options.port = scenario.listen.port();
  if (!client_side.open(client_options)) {
    std::fprintf(stderr, "impair: cannot bind listen port %u: %s\n",
                 static_cast<unsigned>(scenario.listen.port()),
                 std::strerror(client_side.last_error()));
    return 1;
  }

  net::UdpSocket::Options forward_options;
  forward_options.family = scenario.forward.family();
  forward_options.port = 0;
  if (!forward_side.open(forward_options)) {
    std::fprintf(stderr, "impair: cannot open upstream socket: %s\n",
                 std::strerror(forward_side.last_error()));
    return 1;
  }

  // The two directions get independently seeded engines. Sharing one engine
  // would correlate loss between the directions -- a PING would tend to be
  // dropped in the same burst as its PONG, which no real network does and which
  // would quietly break RTT measurement.
  Direction upstream{sim::ImpairmentEngine(scenario.upstream, scenario.seed)};
  Direction downstream{sim::ImpairmentEngine(
      scenario.downstream, scenario.seed ^ 0x9E3779B97F4A7C15ull)};

  std::vector<Pending> queue(kQueueCapacity);
  std::vector<LogRow> log;
  if (!scenario.log_path.empty()) log.reserve(1 << 14);

  // Single client mapping, NAT-style: whoever last sent to the listen port is
  // where return traffic goes. Enough for a benchmark, and stated rather than
  // hidden -- two simultaneous clients would confuse the return path.
  net::Endpoint client{};

  std::array<std::byte, proto::kMaxDatagram> buffer{};
  std::uint64_t serial = 0;

  impair::describe(scenario, stderr);
  std::fprintf(stderr, "impair: relaying %s <-> %s\n",
               client_side.local_endpoint().to_string().c_str(),
               scenario.forward.to_string().c_str());

  // Printed as the very last setup step, and flushed, so a harness can wait for
  // this line instead of guessing at a sleep.
  //
  // This is not cosmetic. The first acceptance run of this tool appeared to lose
  // 8 packets before the model saw them, and the cause was a 0.5 s sleep in the
  // test script that was not long enough for process launch plus setup. A
  // benchmark that races its own subject produces numbers that look like
  // network loss, which is the worst possible failure mode for a measurement
  // tool.
  std::fprintf(stderr, "impair: ready\n");
  std::fflush(stderr);

  const Micros started = now_us();
  const Micros deadline =
      scenario.duration_s > 0.0
          ? started + static_cast<Micros>(scenario.duration_s * 1e6)
          : 0;

  const auto enqueue = [&](Direction& direction, bool to_forward,
                           ByteView datagram, Micros release_us,
                           std::uint64_t row_serial) -> bool {
    Pending* slot = claim_slot(queue);
    if (slot == nullptr) {
      ++direction.queue_full;
      return false;
    }
    slot->occupied = true;
    slot->release_us = release_us;
    slot->length = datagram.size();
    slot->to_forward = to_forward;
    slot->serial = row_serial;
    std::memcpy(slot->data.data(), datagram.data(), datagram.size());
    return true;
  };

  while (g_stop == 0) {
    const Micros now = now_us();
    if (deadline != 0 && now >= deadline) break;

    // Release everything due, and find the next wake-up while we are already
    // walking the queue.
    Micros next_release = 0;
    for (auto& slot : queue) {
      if (!slot.occupied) continue;
      if (slot.release_us <= now) {
        Direction& direction = slot.to_forward ? upstream : downstream;
        const net::Endpoint& target = slot.to_forward ? scenario.forward : client;
        if (target.valid()) {
          auto& socket = slot.to_forward ? forward_side : client_side;
          if (socket.send_to(target, ByteView{slot.data.data(), slot.length})
                  .ok()) {
            ++direction.emitted;
          } else {
            ++direction.send_errors;
          }
        }
        slot.occupied = false;
        continue;
      }
      if (next_release == 0 || slot.release_us < next_release) {
        next_release = slot.release_us;
      }
    }

    int timeout_ms = 20;
    if (next_release != 0) {
      const Micros wait = next_release > now ? next_release - now : 0;
      // Rounded UP, not truncated. poll() only takes milliseconds, so a 400 us
      // wait truncates to 0, poll returns immediately, and the loop spins at
      // 100% CPU until the release is due. Rounding up costs at most 1 ms of
      // release jitter -- well below the ~2 ms the host's own nanosleep
      // contributes -- and eliminates the spin entirely.
      timeout_ms = static_cast<int>((wait + 999ull) / 1'000ull);
      if (timeout_ms > 20) timeout_ms = 20;
      if (timeout_ms < 1) timeout_ms = 1;
    }

    net::UdpSocket* sockets[] = {&client_side, &forward_side};
    bool readable[2] = {false, false};
    const int ready = net::wait_any_readable(sockets, readable, timeout_ms);
    if (ready <= 0) continue;

    for (std::size_t index = 0; index < 2; ++index) {
      if (!readable[index]) continue;
      const bool from_client = index == 0;

      for (;;) {
        auto& socket = from_client ? client_side : forward_side;
        const auto received = socket.recv_from(buffer);
        if (!received.ok()) break;

        if (from_client) client = received.from;

        Direction& direction = from_client ? upstream : downstream;
        ++direction.received;

        const ByteView datagram{buffer.data(), received.bytes};
        const auto decision =
            direction.engine.decide(received.bytes, received.arrival_us);

        LogRow row;
        row.serial = serial++;
        row.to_forward = from_client;
        row.verdict = decision.verdict;
        row.arrival_us = received.arrival_us;
        row.bytes = received.bytes;
        row.duplicate = decision.duplicate;
        row.reordered = decision.reorder_applied;
        classify(datagram, row);

        if (decision.forwarded()) {
          row.delay_us = decision.delay_us;
          row.release_us = received.arrival_us + decision.delay_us;
          if (!enqueue(direction, from_client, datagram, row.release_us,
                       row.serial)) {
            row.verdict = sim::Verdict::DropRateLimit;  // queue overflow
          }
          if (decision.duplicate) {
            enqueue(direction, from_client, datagram,
                    received.arrival_us + decision.duplicate_delay_us,
                    row.serial);
          }
        }

        if (!scenario.log_path.empty()) log.push_back(row);
      }
    }
  }

  // Drain whatever is still held before reporting.
  //
  // This matters more than it looks. With Pareto jitter a single packet can sit
  // in the queue for over a second, so stopping the proxy the moment the sender
  // finishes silently discards the tail -- and those discards then appear in the
  // receiver's loss counter as network loss that the model never asked for. The
  // first acceptance run of this tool reported 12 lost against a ground truth of
  // 10 for exactly that reason.
  //
  // Each packet is still released at its scheduled time, so draining does not
  // distort the delay distribution; it just refuses to stop early. The grace
  // period is bounded so a pathological tail cannot hang the tool, and anything
  // still held when it expires is counted rather than ignored.
  std::uint64_t discarded_at_exit = 0;
  {
    const Micros drain_started = now_us();
    constexpr Micros kMaxDrainUs = 5'000'000;

    for (;;) {
      const Micros now = now_us();
      std::size_t still_held = 0;
      Micros next_release = 0;

      for (auto& slot : queue) {
        if (!slot.occupied) continue;
        if (slot.release_us <= now) {
          Direction& direction = slot.to_forward ? upstream : downstream;
          const net::Endpoint& target =
              slot.to_forward ? scenario.forward : client;
          if (target.valid()) {
            auto& socket = slot.to_forward ? forward_side : client_side;
            if (socket.send_to(target, ByteView{slot.data.data(), slot.length})
                    .ok()) {
              ++direction.emitted;
            } else {
              ++direction.send_errors;
            }
          }
          slot.occupied = false;
          continue;
        }
        ++still_held;
        if (next_release == 0 || slot.release_us < next_release) {
          next_release = slot.release_us;
        }
      }

      if (still_held == 0) break;

      if (now - drain_started > kMaxDrainUs) {
        for (auto& slot : queue) {
          if (slot.occupied) {
            slot.occupied = false;
            ++discarded_at_exit;
          }
        }
        break;
      }

      const Micros wait = next_release > now ? next_release - now : 0;
      timespec request{};
      request.tv_sec = static_cast<time_t>(wait / 1'000'000ull);
      request.tv_nsec = static_cast<long>((wait % 1'000'000ull) * 1'000ull);
      ::nanosleep(&request, nullptr);
    }

    if (discarded_at_exit != 0) {
      std::fprintf(stderr,
                   "impair: %llu packets still queued after a %llu s drain and "
                   "were discarded\n",
                   static_cast<unsigned long long>(discarded_at_exit),
                   static_cast<unsigned long long>(kMaxDrainUs / 1'000'000));
    }
  }

  if (!scenario.log_path.empty()) write_log(scenario.log_path, log);

  if (scenario.json) {
    report_json(scenario, upstream, downstream, log.size(), discarded_at_exit);
  } else {
    report_human(scenario, upstream, downstream, log.size(),
                 discarded_at_exit);
  }
  return 0;
}
