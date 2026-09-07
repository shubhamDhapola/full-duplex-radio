#include "scenario.hpp"

#include <charconv>
#include <cstring>
#include <fstream>
#include <string_view>

namespace impair {
namespace {

using radio::sim::Impairment;
using radio::sim::JitterShape;

std::string_view trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
    text.remove_prefix(1);
  }
  while (!text.empty() &&
         (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
    text.remove_suffix(1);
  }
  return text;
}

bool parse_double(std::string_view text, double& out) {
  std::string owned(text);
  char* end = nullptr;
  const double value = std::strtod(owned.c_str(), &end);
  if (end != owned.c_str() + owned.size() || owned.empty()) return false;
  out = value;
  return true;
}

template <class T>
bool parse_integer(std::string_view text, T& out) {
  const auto* begin = text.data();
  const auto* end = begin + text.size();
  const auto result = std::from_chars(begin, end, out);
  return result.ec == std::errc{} && result.ptr == end;
}

bool parse_shape(std::string_view text, JitterShape& out) {
  if (text == "none")
    out = JitterShape::None;
  else if (text == "uniform")
    out = JitterShape::Uniform;
  else if (text == "normal")
    out = JitterShape::Normal;
  else if (text == "pareto")
    out = JitterShape::Pareto;
  else
    return false;
  return true;
}

// Applies one key to one direction's impairment. Returns false for an unknown
// key, so a typo in a scenario file is an error rather than a silently ignored
// line -- a misspelled `loss_percnt` would otherwise produce a clean-looking
// run with no loss at all.
bool apply_key(Impairment& target, std::string_view key,
               std::string_view value) {
  double number = 0.0;

  if (key == "base_delay_ms") {
    if (!parse_double(value, number)) return false;
    target.base_delay_us = static_cast<radio::Micros>(number * 1'000.0);
    return true;
  }
  if (key == "jitter_ms") {
    if (!parse_double(value, number)) return false;
    target.jitter_us = static_cast<radio::Micros>(number * 1'000.0);
    return true;
  }
  if (key == "jitter_shape") return parse_shape(value, target.jitter_shape);
  if (key == "loss_percent") return parse_double(value, target.loss_percent);
  if (key == "burst_loss_percent") {
    return parse_double(value, target.burst_loss_percent);
  }
  if (key == "burst_mean_length") {
    return parse_double(value, target.burst_mean_length);
  }
  if (key == "duplicate_percent") {
    return parse_double(value, target.duplicate_percent);
  }
  if (key == "reorder_percent") {
    return parse_double(value, target.reorder_percent);
  }
  if (key == "reorder_extra_ms") {
    if (!parse_double(value, number)) return false;
    target.reorder_extra_us = static_cast<radio::Micros>(number * 1'000.0);
    return true;
  }
  if (key == "rate_kbps") {
    if (!parse_double(value, number)) return false;
    target.rate_bps = static_cast<std::uint64_t>(number * 1'000.0);
    return true;
  }
  if (key == "bucket_bytes") return parse_integer(value, target.bucket_bytes);
  return false;
}

}  // namespace

bool load_scenario_file(const std::string& path, Scenario& out) {
  std::ifstream file(path);
  if (!file) {
    std::fprintf(stderr, "impair: cannot open scenario '%s'\n", path.c_str());
    return false;
  }

  std::string line;
  int line_number = 0;
  while (std::getline(file, line)) {
    ++line_number;
    std::string_view text = trim(line);
    if (text.empty() || text.front() == '#') continue;

    const auto equals = text.find('=');
    if (equals == std::string_view::npos) {
      std::fprintf(stderr, "impair: %s:%d: expected key = value\n",
                   path.c_str(), line_number);
      return false;
    }

    std::string_view key = trim(text.substr(0, equals));
    const std::string_view value = trim(text.substr(equals + 1));

    if (key == "name") {
      out.name.assign(value);
      continue;
    }

    // Direction prefixes. Unprefixed applies to both.
    bool ok = false;
    if (key.starts_with("up.")) {
      ok = apply_key(out.upstream, key.substr(3), value);
    } else if (key.starts_with("down.")) {
      ok = apply_key(out.downstream, key.substr(5), value);
    } else {
      ok = apply_key(out.upstream, key, value) &&
           apply_key(out.downstream, key, value);
    }

    if (!ok) {
      std::fprintf(stderr, "impair: %s:%d: bad key or value '%.*s'\n",
                   path.c_str(), line_number, static_cast<int>(text.size()),
                   text.data());
      return false;
    }
  }
  return true;
}

void print_usage(std::FILE* out) {
  std::fprintf(out, R"(impair - reproducible UDP impairment proxy

USAGE
  impair --listen PORT --forward HOST:PORT [--scenario FILE] [--seed N]
         [--log FILE] [--duration S] [--json] [key=value ...]

Traffic sent to --listen is relayed to --forward and back, with loss, delay,
jitter, duplication and reordering applied from a seeded model. The same seed
replays a byte-identical pattern, which is what makes an A/B comparison
attributable to the change under test rather than to a different loss pattern.

OPTIONS
  --listen PORT       Local port clients connect to (required)
  --forward HOST:PORT Where to relay traffic (required)
  --scenario FILE     Load settings from a scenario file
  --seed N            PRNG seed (default 1)
  --log FILE          Write the ground-truth event log as CSV
  --duration S        Stop after S seconds (default: until interrupted)
  --json              Emit the summary as JSON
  -h, --help          This message

SCENARIO KEYS  (also accepted on the command line as key=value)
  base_delay_ms       Fixed delay added to every packet
  jitter_ms           Magnitude of the variable component
  jitter_shape        none | uniform | normal | pareto
  loss_percent        Independent per-packet loss
  burst_loss_percent  Overall loss delivered in bursts (Gilbert-Elliott)
  burst_mean_length   Mean consecutive packets lost per burst
  duplicate_percent   Packets emitted twice
  reorder_percent     Packets given extra delay so they overtake successors
  reorder_extra_ms    How much extra (default 15)
  rate_kbps           Token-bucket link limit, 0 = unlimited
  bucket_bytes        Burst allowance for the rate limit

  Prefix a key with `up.` or `down.` to set only one direction. `up.` is
  client -> forward. Unprefixed keys set both.

EXAMPLES
  impair --listen 47000 --forward 127.0.0.1:47001 \
         --scenario benchmarks/scenarios/poor.conf --seed 42 --log gt.csv

  impair --listen 47000 --forward 127.0.0.1:47001 \
         burst_loss_percent=5 burst_mean_length=4 base_delay_ms=30
)");
}

void describe(const Scenario& scenario, std::FILE* out) {
  const auto show = [out](const char* label, const Impairment& i) {
    std::fprintf(out, "  %-10s delay %.1f ms +/- %.1f ms (%s)", label,
                 static_cast<double>(i.base_delay_us) / 1000.0,
                 static_cast<double>(i.jitter_us) / 1000.0,
                 radio::sim::to_string(i.jitter_shape));
    if (i.loss_percent > 0.0)
      std::fprintf(out, ", loss %.2f%%", i.loss_percent);
    if (i.burst_loss_percent > 0.0) {
      std::fprintf(out, ", burst %.2f%% x%.1f", i.burst_loss_percent,
                   i.burst_mean_length);
    }
    if (i.duplicate_percent > 0.0) {
      std::fprintf(out, ", dup %.2f%%", i.duplicate_percent);
    }
    if (i.reorder_percent > 0.0) {
      std::fprintf(out, ", reorder %.2f%%", i.reorder_percent);
    }
    if (i.rate_bps > 0) {
      std::fprintf(out, ", limit %.0f kbps",
                   static_cast<double>(i.rate_bps) / 1000.0);
    }
    std::fprintf(out, "\n");
  };

  std::fprintf(out, "impair scenario '%s' seed %llu\n", scenario.name.c_str(),
               static_cast<unsigned long long>(scenario.seed));
  show("upstream", scenario.upstream);
  show("downstream", scenario.downstream);
}

std::optional<Scenario> parse_arguments(int argc, char** argv) {
  Scenario scenario;
  std::uint16_t listen_port = 0;

  const auto value_for = [&](int& index, std::string_view flag) -> const char* {
    if (index + 1 >= argc) {
      std::fprintf(stderr, "impair: %.*s requires a value\n",
                   static_cast<int>(flag.size()), flag.data());
      return nullptr;
    }
    return argv[++index];
  };

  // Scenario file first so command-line key=value pairs can override it.
  for (int i = 1; i < argc; ++i) {
    if (std::string_view(argv[i]) == "--scenario" && i + 1 < argc) {
      if (!load_scenario_file(argv[i + 1], scenario)) return std::nullopt;
    }
  }

  for (int i = 1; i < argc; ++i) {
    const std::string_view flag = argv[i];
    const char* value = nullptr;

    if (flag == "-h" || flag == "--help") {
      scenario.help = true;
      return scenario;
    }
    if (flag == "--json") {
      scenario.json = true;
      continue;
    }
    if (flag == "--scenario") {
      ++i;  // already consumed above
      continue;
    }
    if (flag == "--listen") {
      if ((value = value_for(i, flag)) == nullptr) return std::nullopt;
      if (!parse_integer(std::string_view(value), listen_port)) {
        std::fprintf(stderr, "impair: bad listen port '%s'\n", value);
        return std::nullopt;
      }
      continue;
    }
    if (flag == "--forward") {
      if ((value = value_for(i, flag)) == nullptr) return std::nullopt;
      const auto parsed = radio::net::Endpoint::parse_with_port(value);
      if (!parsed) {
        std::fprintf(stderr, "impair: bad forward endpoint '%s'\n", value);
        return std::nullopt;
      }
      scenario.forward = *parsed;
      continue;
    }
    if (flag == "--seed") {
      if ((value = value_for(i, flag)) == nullptr) return std::nullopt;
      if (!parse_integer(std::string_view(value), scenario.seed)) {
        std::fprintf(stderr, "impair: bad seed '%s'\n", value);
        return std::nullopt;
      }
      continue;
    }
    if (flag == "--log") {
      if ((value = value_for(i, flag)) == nullptr) return std::nullopt;
      scenario.log_path = value;
      continue;
    }
    if (flag == "--duration") {
      if ((value = value_for(i, flag)) == nullptr) return std::nullopt;
      if (!parse_double(std::string_view(value), scenario.duration_s)) {
        std::fprintf(stderr, "impair: bad duration '%s'\n", value);
        return std::nullopt;
      }
      continue;
    }

    // Bare key=value overrides, so a scenario can be tweaked without editing a
    // file.
    const auto equals = flag.find('=');
    if (equals != std::string_view::npos && !flag.starts_with("--")) {
      std::string_view key = flag.substr(0, equals);
      const std::string_view raw = flag.substr(equals + 1);
      bool ok = false;
      if (key.starts_with("up.")) {
        ok = apply_key(scenario.upstream, key.substr(3), raw);
      } else if (key.starts_with("down.")) {
        ok = apply_key(scenario.downstream, key.substr(5), raw);
      } else {
        ok = apply_key(scenario.upstream, key, raw) &&
             apply_key(scenario.downstream, key, raw);
      }
      if (!ok) {
        std::fprintf(stderr, "impair: bad key or value '%.*s'\n",
                     static_cast<int>(flag.size()), flag.data());
        return std::nullopt;
      }
      continue;
    }

    std::fprintf(stderr, "impair: unknown option '%.*s'\n",
                 static_cast<int>(flag.size()), flag.data());
    return std::nullopt;
  }

  if (listen_port == 0) {
    std::fprintf(stderr, "impair: --listen PORT is required\n");
    return std::nullopt;
  }
  if (!scenario.forward.valid()) {
    std::fprintf(stderr, "impair: --forward HOST:PORT is required\n");
    return std::nullopt;
  }

  scenario.listen = *radio::net::Endpoint::parse(
      scenario.forward.family() == radio::net::Endpoint::Family::V6 ? "::"
                                                                    : "0.0.0.0",
      listen_port);
  return scenario;
}

}  // namespace impair
