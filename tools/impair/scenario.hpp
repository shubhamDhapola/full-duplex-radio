#pragma once

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>

#include "radio/endpoint.hpp"
#include "radio/impairment.hpp"

namespace impair {

struct Scenario {
  std::string name = "unnamed";

  // Applied to client -> forward traffic and forward -> client traffic
  // respectively. Unprefixed keys in a scenario file set both; `up.` and `down.`
  // prefixes override one direction, which is what lets a scenario reproduce the
  // asymmetric path that clock-offset estimation cannot see through.
  radio::sim::Impairment upstream{};
  radio::sim::Impairment downstream{};

  radio::net::Endpoint listen{};
  radio::net::Endpoint forward{};
  std::uint64_t seed = 1;
  double duration_s = 0.0;  // 0 = until interrupted
  std::string log_path;
  bool json = false;
  bool help = false;
};

[[nodiscard]] std::optional<Scenario> parse_arguments(int argc, char** argv);
[[nodiscard]] bool load_scenario_file(const std::string& path, Scenario& out);
void print_usage(std::FILE* out);
void describe(const Scenario& scenario, std::FILE* out);

}  // namespace impair
