#include "options.hpp"

#include <charconv>
#include <cstring>
#include <string_view>

#include "scenario.hpp"

namespace radiobench {
namespace {

// Parses an integral argument, rejecting trailing junk. `--count 20x` is a typo
// worth reporting rather than silently reading as 20.
template <class T>
bool parse_integer(std::string_view text, T& out) {
  const auto* begin = text.data();
  const auto* end = begin + text.size();
  const auto result = std::from_chars(begin, end, out);
  return result.ec == std::errc{} && result.ptr == end;
}

bool parse_double(std::string_view text, double& out) {
  // from_chars for floating point is not available in libc++ of every version
  // we target, so strtod with an explicit end check does the same job.
  std::string owned(text);
  char* end = nullptr;
  const double value = std::strtod(owned.c_str(), &end);
  if (end != owned.c_str() + owned.size()) return false;
  out = value;
  return true;
}

}  // namespace

void print_usage(std::FILE* out) {
  std::fprintf(out, R"(radiobench - protocol v1 transport lab

USAGE
  radiobench respond [--port N] [--json]
  radiobench ping    --peer HOST:PORT [--count N] [--interval MS] [--timeout MS] [--json]
  radiobench send    --peer HOST:PORT [--rate PPS] [--size B] [--duration S]
                     [--seed N] [--trace FILE] [--json]
  radiobench wavloop --in FILE.wav [--out FILE.wav] [--scenario FILE] [--seed N]
                     [--fec] [--bitrate BPS] [--expected-loss PCT]
                     [--target-delay MS] [--trace FILE] [--json] [key=value ...]

MODES
  respond   Act as a peer: reply to PING with PONG and account for received
            AUDIO streams (loss, duplicates, reordering, jitter). Runs until
            interrupted.
  ping      Measure RTT and estimate the clock offset from the four-timestamp
            exchange of specification section 4.1.
  send      Transmit a paced AUDIO stream with sequence numbers and timestamps.
  wavloop   The whole media pipeline in one process, in real time:
            wav -> Opus -> UDP -> impairment -> jitter buffer -> decode -> wav.
            One clock on both ends, so end-to-end latency is measured exactly
            rather than estimated through a clock offset.

OPTIONS
  --peer HOST:PORT   Remote endpoint. IPv6 in brackets: [fe80::1%%en0]:47000
  --port N           Local port to bind (default %u)
  --count N          Probes to send (default 20)
  --interval MS      Spacing between probes (default 200)
  --timeout MS       Time to wait for each PONG (default 1000)
  --rate PPS         Packets per second (default 50, i.e. 20 ms frames)
  --size B           Payload bytes per packet (default 80)
  --duration S       Seconds to transmit (default 10)
  --seed N           Fix stream id / sequence / timestamp start for reproducibility
  --in FILE          wavloop: input WAV, 16-bit PCM at 48 kHz
  --out FILE         wavloop: write the playout stream here (default: none)
  --scenario FILE    wavloop: impairment scenario, same format as `impair`
  --fec              wavloop: enable Opus in-band FEC
  --bitrate BPS      wavloop: encoder bitrate (default 32000)
  --expected-loss P  wavloop: loss hint driving FEC redundancy (default 0)
  --target-delay MS  wavloop: jitter buffer depth (default 60)
  --trace FILE       Write per-packet stage timestamps as CSV
  --json             Emit the report as JSON instead of a table
  -h, --help         This message

  wavloop also accepts bare `key=value` impairment settings, exactly as
  `impair` does; see `impair --help` for the key list.

EXAMPLES
  # terminal 1
  radiobench respond --port 47000

  # terminal 2
  radiobench ping --peer 127.0.0.1:47000 --count 50 --interval 100
  radiobench send --peer 127.0.0.1:47000 --rate 50 --duration 5 --seed 42

  # the M1 comparison: one impairment pattern, FEC off then on
  radiobench wavloop --in speech.wav --out off.wav --seed 42 \
                     --scenario benchmarks/scenarios/poor.conf
  radiobench wavloop --in speech.wav --out on.wav  --seed 42 \
                     --scenario benchmarks/scenarios/poor.conf \
                     --fec --expected-loss 20
)",
               static_cast<unsigned>(kDefaultPort));
}

std::optional<Options> parse_options(int argc, char** argv) {
  Options options;

  if (argc < 2) {
    std::fprintf(stderr, "radiobench: no mode given\n\n");
    return std::nullopt;
  }

  const std::string_view mode = argv[1];
  if (mode == "-h" || mode == "--help" || mode == "help") {
    options.help = true;
    return options;
  }
  if (mode == "respond") {
    options.mode = Options::Mode::Respond;
  } else if (mode == "ping") {
    options.mode = Options::Mode::Ping;
  } else if (mode == "send") {
    options.mode = Options::Mode::Send;
  } else if (mode == "wavloop") {
    options.mode = Options::Mode::WavLoop;
  } else {
    std::fprintf(stderr, "radiobench: unknown mode '%.*s'\n\n",
                 static_cast<int>(mode.size()), mode.data());
    return std::nullopt;
  }

  // Every flag that takes a value shares this check, so a missing value is
  // reported as such rather than reading the next flag as the value.
  const auto value_for = [&](int& index, std::string_view flag) -> const char* {
    if (index + 1 >= argc) {
      std::fprintf(stderr, "radiobench: %.*s requires a value\n",
                   static_cast<int>(flag.size()), flag.data());
      return nullptr;
    }
    return argv[++index];
  };

  for (int i = 2; i < argc; ++i) {
    const std::string_view flag = argv[i];

    if (flag == "-h" || flag == "--help") {
      options.help = true;
      return options;
    }
    if (flag == "--json") {
      options.json = true;
      continue;
    }

    const char* value = nullptr;

    if (flag == "--peer") {
      if ((value = value_for(i, flag)) == nullptr) return std::nullopt;
      const auto parsed = radio::net::Endpoint::parse_with_port(value);
      if (!parsed) {
        std::fprintf(
            stderr,
            "radiobench: could not parse peer '%s'. Expected a numeric "
            "address, e.g. 192.168.1.14:47000 or [fe80::1]:47000\n",
            value);
        return std::nullopt;
      }
      options.peer = *parsed;
      options.family = options.peer.family();
      continue;
    }
    if (flag == "--port") {
      if ((value = value_for(i, flag)) == nullptr) return std::nullopt;
      if (!parse_integer(value, options.port)) {
        std::fprintf(stderr, "radiobench: bad port '%s'\n", value);
        return std::nullopt;
      }
      continue;
    }
    if (flag == "--count") {
      if ((value = value_for(i, flag)) == nullptr) return std::nullopt;
      if (!parse_integer(value, options.count) || options.count == 0) {
        std::fprintf(stderr, "radiobench: bad count '%s'\n", value);
        return std::nullopt;
      }
      continue;
    }
    if (flag == "--interval") {
      if ((value = value_for(i, flag)) == nullptr) return std::nullopt;
      if (!parse_integer(value, options.interval_ms)) {
        std::fprintf(stderr, "radiobench: bad interval '%s'\n", value);
        return std::nullopt;
      }
      continue;
    }
    if (flag == "--timeout") {
      if ((value = value_for(i, flag)) == nullptr) return std::nullopt;
      if (!parse_integer(value, options.timeout_ms)) {
        std::fprintf(stderr, "radiobench: bad timeout '%s'\n", value);
        return std::nullopt;
      }
      continue;
    }
    if (flag == "--rate") {
      if ((value = value_for(i, flag)) == nullptr) return std::nullopt;
      if (!parse_integer(value, options.rate_pps) || options.rate_pps == 0) {
        std::fprintf(stderr, "radiobench: bad rate '%s'\n", value);
        return std::nullopt;
      }
      continue;
    }
    if (flag == "--size") {
      if ((value = value_for(i, flag)) == nullptr) return std::nullopt;
      if (!parse_integer(value, options.payload_bytes) ||
          options.payload_bytes == 0) {
        std::fprintf(stderr, "radiobench: bad size '%s'\n", value);
        return std::nullopt;
      }
      continue;
    }
    if (flag == "--duration") {
      if ((value = value_for(i, flag)) == nullptr) return std::nullopt;
      if (!parse_double(value, options.duration_s) ||
          options.duration_s <= 0.0) {
        std::fprintf(stderr, "radiobench: bad duration '%s'\n", value);
        return std::nullopt;
      }
      continue;
    }
    if (flag == "--seed") {
      if ((value = value_for(i, flag)) == nullptr) return std::nullopt;
      std::uint32_t seed = 0;
      if (!parse_integer(value, seed)) {
        std::fprintf(stderr, "radiobench: bad seed '%s'\n", value);
        return std::nullopt;
      }
      options.seed = seed;
      continue;
    }
    if (flag == "--trace") {
      if ((value = value_for(i, flag)) == nullptr) return std::nullopt;
      options.trace_path = value;
      continue;
    }
    if (flag == "--in") {
      if ((value = value_for(i, flag)) == nullptr) return std::nullopt;
      options.wav_in = value;
      continue;
    }
    if (flag == "--out") {
      if ((value = value_for(i, flag)) == nullptr) return std::nullopt;
      options.wav_out = value;
      continue;
    }
    if (flag == "--scenario") {
      if ((value = value_for(i, flag)) == nullptr) return std::nullopt;
      impair::Scenario scenario;
      if (!impair::load_scenario_file(value, scenario)) return std::nullopt;
      // Upstream only. wavloop models one direction, because the thing it
      // measures -- a frame's journey from capture to playout -- only travels
      // one way. A return path would be M5's problem, not this one's.
      options.impairment = scenario.upstream;
      options.scenario_name = scenario.name;
      continue;
    }
    if (flag == "--fec") {
      options.fec = true;
      continue;
    }
    if (flag == "--bitrate") {
      if ((value = value_for(i, flag)) == nullptr) return std::nullopt;
      if (!parse_integer(value, options.bitrate_bps) ||
          options.bitrate_bps <= 0) {
        std::fprintf(stderr, "radiobench: bad bitrate '%s'\n", value);
        return std::nullopt;
      }
      continue;
    }
    if (flag == "--expected-loss") {
      if ((value = value_for(i, flag)) == nullptr) return std::nullopt;
      if (!parse_integer(value, options.expected_loss_percent) ||
          options.expected_loss_percent < 0 ||
          options.expected_loss_percent > 100) {
        std::fprintf(stderr, "radiobench: bad expected loss '%s'\n", value);
        return std::nullopt;
      }
      continue;
    }
    if (flag == "--target-delay") {
      if ((value = value_for(i, flag)) == nullptr) return std::nullopt;
      if (!parse_integer(value, options.target_delay_ms)) {
        std::fprintf(stderr, "radiobench: bad target delay '%s'\n", value);
        return std::nullopt;
      }
      continue;
    }

    // Bare `key=value` impairment settings, the same ones `impair` takes. They
    // come after the scenario file in the argument list precisely so they can
    // override it, which is how the matrix sweeps one knob at a time.
    const auto equals = flag.find('=');
    if (options.mode == Options::Mode::WavLoop &&
        equals != std::string_view::npos && equals != 0) {
      if (!impair::apply_impairment_key(options.impairment,
                                        flag.substr(0, equals),
                                        flag.substr(equals + 1))) {
        std::fprintf(stderr, "radiobench: bad setting '%.*s'\n",
                     static_cast<int>(flag.size()), flag.data());
        return std::nullopt;
      }
      continue;
    }

    std::fprintf(stderr, "radiobench: unknown option '%.*s'\n",
                 static_cast<int>(flag.size()), flag.data());
    return std::nullopt;
  }

  if ((options.mode == Options::Mode::Ping ||
       options.mode == Options::Mode::Send) &&
      !options.peer.valid()) {
    std::fprintf(stderr, "radiobench: this mode requires --peer HOST:PORT\n");
    return std::nullopt;
  }

  if (options.mode == Options::Mode::WavLoop) {
    if (options.wav_in.empty()) {
      std::fprintf(stderr, "radiobench: wavloop requires --in FILE.wav\n");
      return std::nullopt;
    }
    // Opus emits no redundancy at all unless it also believes packets are
    // being lost, so --fec alone is a silent no-op and the encoder rejects it.
    // Defaulting the hint from the scenario would be convenient and wrong: the
    // sender does not know the loss rate, it estimates it, and pretending
    // otherwise would flatter every FEC result in the matrix.
    if (options.fec && options.expected_loss_percent == 0) {
      std::fprintf(stderr,
                   "radiobench: --fec needs --expected-loss PCT above 0, "
                   "or Opus emits no redundancy at all\n");
      return std::nullopt;
    }
  }

  return options;
}

}  // namespace radiobench
