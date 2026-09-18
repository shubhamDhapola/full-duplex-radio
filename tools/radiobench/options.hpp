#pragma once

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>

#include "radio/endpoint.hpp"
#include "radio/impairment.hpp"

namespace radiobench {

inline constexpr std::uint16_t kDefaultPort = 47'000;

struct Options {
  enum class Mode { None, Respond, Ping, Send, WavLoop };

  Mode mode = Mode::None;

  radio::net::Endpoint peer{};
  std::uint16_t port = kDefaultPort;
  radio::net::Endpoint::Family family = radio::net::Endpoint::Family::V4;

  std::uint32_t count = 20;          // ping: probes to send
  std::uint32_t interval_ms = 200;   // ping: spacing between probes
  std::uint32_t timeout_ms = 1'000;  // ping: how long to wait for a PONG

  std::uint32_t rate_pps = 50;  // send: packets per second (50 = 20 ms frames)
  std::uint32_t payload_bytes = 80;  // send: ~a 32 kbps Opus frame
  double duration_s = 10.0;          // send: how long to transmit

  // Fixes the stream id, sequence and timestamp start values. Two runs with the
  // same seed produce byte-identical packets, which is what makes an A/B
  // comparison against the impairment proxy meaningful rather than suggestive.
  std::optional<std::uint32_t> seed;

  // ------------------------------------------------------------- wavloop

  std::string wav_in;
  std::string wav_out;

  // The impairment applied between send and playout. Loaded from the same
  // scenario files `impair` reads, so a wavloop result and a proxy result are
  // describing the same network.
  radio::sim::Impairment impairment{};
  std::string scenario_name = "perfect";

  std::int32_t bitrate_bps = 32'000;
  bool fec = false;
  int expected_loss_percent = 0;

  // The jitter buffer's target delay, which is the latency knob for the whole
  // pipeline. Held in milliseconds here only because that is what a person
  // types.
  std::uint32_t target_delay_ms = 60;

  std::string trace_path;  // empty = no trace file
  bool json = false;
  bool help = false;
};

[[nodiscard]] std::optional<Options> parse_options(int argc, char** argv);
void print_usage(std::FILE* out);

}  // namespace radiobench
