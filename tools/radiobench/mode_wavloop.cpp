// The whole media pipeline in one process, in real time.
//
//   wav -> Opus -> UDP -> impairment -> jitter buffer -> decode -> wav
//
// WHY ONE PROCESS, AND WHY THIS IS THE REFERENCE MEASUREMENT
//
// Every latency figure this project publishes is a difference between two
// timestamps. Across two devices those come from two clocks, and the offset
// between them is only knowable to within +/- rtt/2 (see ClockSync). On a LAN
// that is a few hundred microseconds of uncertainty on a figure of interest
// around 100 ms -- small, but it is *systematic*, and it never goes away.
//
// Here both ends share one CLOCK_MONOTONIC_RAW. Capture-to-playout is a plain
// subtraction with no estimator in it at all. That number becomes the reference
// every on-device measurement in M2 is compared against: when the phone reports
// 180 ms, the question is what the phone added on top of this, not whether the
// clocks agreed.
//
// WHY IT RUNS IN REAL TIME RATHER THAN AS FAST AS IT CAN
//
// A virtual clock would finish a 12 second file in a fraction of a second and
// produce identical audio. It could not produce a latency budget: encode cost,
// scheduling jitter, the pacer's accuracy and the decode cost inside the pull
// are all properties of the machine running, and they are exactly what the
// per-stage report is for.
//
// WHERE THE IMPAIRMENT SITS, AND WHAT THAT DOES TO THE TRACE
//
// There is no second host to hold a packet in flight, so a datagram is really
// sent and really received over loopback -- about 30 microseconds -- and the
// modelled network delay is applied *after* recvmsg, by holding the datagram
// in a release queue until its due time.
//
// That is a deliberate choice about where to put the instrument.
// Stage::Received keeps the meaning trace.hpp gives it (recvmsg returned), so
// the CSV does not lie about its own columns, and the modelled network appears
// as the Received -> JitterIn span. The consequence to remember when reading a
// report: `sent -> received` is the loopback, and `network hold` is the model.
// On two real hosts those merge into one span.
#include <cstdio>

#include "modes.hpp"
#include "options.hpp"

#ifndef RADIO_HAVE_OPUS

namespace radiobench {

int run_wavloop(const Options&) {
  std::fprintf(stderr,
               "radiobench: wavloop needs the Opus audio layer, which this "
               "build does not have.\n"
               "Reconfigure with -DRADIO_WITH_OPUS=ON and rebuild.\n");
  return 2;
}

}  // namespace radiobench

#else

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "radio/clock.hpp"
#include "radio/histogram.hpp"
#include "radio/impairment.hpp"
#include "radio/jitter_buffer.hpp"
#include "radio/opus_codec.hpp"
#include "radio/pacer.hpp"
#include "radio/proto.hpp"
#include "radio/serial.hpp"
#include "radio/trace.hpp"
#include "radio/udp_socket.hpp"
#include "radio/wav.hpp"
#include "util.hpp"

namespace radiobench {
namespace {

using namespace radio;

// Datagrams the impairment model is holding until their release time.
//
// 512 slots is roughly ten seconds of one-way audio at 50 pps, which is far
// more than any plausible delay setting needs. It is fixed and preallocated for
// the same reason everything else on this path is: a reallocation mid-run would
// land in the measurement as network jitter.
constexpr std::size_t kHeldCapacity = 512;

struct Held {
  std::array<std::byte, proto::kMaxDatagram> bytes{};
  std::size_t length = 0;
  Micros release_us = 0;
  bool occupied = false;
};

struct Stages {
  Histogram encode;   // Captured  -> Encoded
  Histogram send;     // Queued    -> Sent
  Histogram wire;     // Sent      -> Received   (real loopback)
  Histogram network;  // Received  -> JitterIn   (the model)
  Histogram buffer;   // JitterIn  -> JitterOut
  Histogram decode;   // JitterOut -> Decoded
  Histogram total;    // Captured  -> Played
};

struct Counts {
  std::uint64_t frames_in = 0;
  std::uint64_t encode_failed = 0;
  std::uint64_t send_failed = 0;
  std::uint64_t datagrams_received = 0;
  std::uint64_t parse_rejected = 0;
  std::uint64_t held_overflow = 0;
  std::uint64_t ticks = 0;
  Micros started_us = 0;
  Micros finished_us = 0;
};

// Records a span only when both ends were stamped. A frame that was dropped in
// the network has no Received stamp, and feeding a zero into the histogram
// would drag every percentile toward zero -- which is the single easiest way to
// publish a latency figure that is better than the system can achieve.
void record_span(Histogram& histogram, const TraceRecord& record, Stage from,
                 Stage to) {
  if (!record.has(from) || !record.has(to)) return;
  const std::int64_t span = record.span_us(from, to);
  if (span < 0) return;
  histogram.record(static_cast<std::uint64_t>(span));
}

void print_stage_row(const char* label, const Histogram& histogram) {
  if (histogram.empty()) {
    std::printf("    %-22s %8s %8s %8s %8s\n", label, "-", "-", "-", "-");
    return;
  }
  const auto snapshot = histogram.snapshot();
  std::printf("    %-22s %8llu %8llu %8llu %8llu\n", label,
              static_cast<unsigned long long>(snapshot.p50),
              static_cast<unsigned long long>(snapshot.p95),
              static_cast<unsigned long long>(snapshot.p99),
              static_cast<unsigned long long>(snapshot.max));
}

void describe_impairment(const sim::Impairment& impairment) {
  std::printf("                 delay %.1f ms +/- %.1f ms (%s)",
              static_cast<double>(impairment.base_delay_us) / 1000.0,
              static_cast<double>(impairment.jitter_us) / 1000.0,
              sim::to_string(impairment.jitter_shape));
  if (impairment.loss_percent > 0.0) {
    std::printf(", loss %.2f%%", impairment.loss_percent);
  }
  if (impairment.burst_loss_percent > 0.0) {
    std::printf(", burst %.2f%% x%.1f", impairment.burst_loss_percent,
                impairment.burst_mean_length);
  }
  if (impairment.duplicate_percent > 0.0) {
    std::printf(", dup %.2f%%", impairment.duplicate_percent);
  }
  if (impairment.reorder_percent > 0.0) {
    std::printf(", reorder %.2f%%", impairment.reorder_percent);
  }
  if (impairment.rate_bps > 0) {
    std::printf(", limit %.0f kbps",
                static_cast<double>(impairment.rate_bps) / 1000.0);
  }
  std::printf("\n");
}

void report_human(const Options& options, const Counts& counts,
                  const audio::WavInfo& info, const sim::ImpairmentEngine& net,
                  const audio::JitterBuffer& jitter, const Pacer& pacer,
                  const Stages& stages, std::uint64_t seed,
                  std::size_t out_frames) {
  const auto& impairment_counts = net.counters();
  const auto& queue = jitter.queue();

  std::printf("\nradiobench wavloop\n");
  std::printf("  input          %s  %.2f s, %llu frames, %u Hz\n",
              options.wav_in.c_str(), info.duration_seconds(),
              static_cast<unsigned long long>(counts.frames_in),
              info.sample_rate_hz);
  std::printf("  encoder        %.1f kbps, FEC %s",
              static_cast<double>(options.bitrate_bps) / 1000.0,
              options.fec ? "on" : "off");
  if (options.fec) {
    std::printf(" (expected loss %d%%)", options.expected_loss_percent);
  }
  std::printf("\n");
  std::printf("  jitter buffer  %u ms target\n", options.target_delay_ms);
  std::printf("  scenario       %s  seed %llu\n", options.scenario_name.c_str(),
              static_cast<unsigned long long>(seed));
  describe_impairment(options.impairment);

  std::printf("\n  network (modelled)\n");
  std::printf("    considered   %llu, forwarded %llu, dropped %llu (%.2f%%)\n",
              static_cast<unsigned long long>(impairment_counts.considered),
              static_cast<unsigned long long>(impairment_counts.forwarded),
              static_cast<unsigned long long>(net.dropped_total()),
              net.measured_loss_fraction() * 100.0);
  std::printf(
      "    independent %llu, burst %llu, rate limit %llu\n",
      static_cast<unsigned long long>(impairment_counts.dropped_independent),
      static_cast<unsigned long long>(impairment_counts.dropped_burst),
      static_cast<unsigned long long>(impairment_counts.dropped_rate_limit));
  std::printf(
      "    duplicated   %llu, reordered %llu, bursts %llu\n",
      static_cast<unsigned long long>(impairment_counts.duplicated),
      static_cast<unsigned long long>(impairment_counts.reordered),
      static_cast<unsigned long long>(impairment_counts.bursts_entered));

  std::printf("\n  playout (%llu ticks)\n",
              static_cast<unsigned long long>(counts.ticks));
  std::printf("    from packet  %llu\n",
              static_cast<unsigned long long>(jitter.from_packet()));
  std::printf("    fec recovery %llu\n",
              static_cast<unsigned long long>(jitter.fec_recovered()));
  std::printf("    concealed    %llu\n",
              static_cast<unsigned long long>(jitter.concealed()));
  std::printf("    silence      %llu  (gave up concealing %llu times)\n",
              static_cast<unsigned long long>(jitter.silence()),
              static_cast<unsigned long long>(jitter.muted()));

  // Late and lost are printed next to each other on purpose. They sound
  // identical and have opposite fixes: late says the buffer is too shallow for
  // this network, gaps say the network lost the packet outright.
  std::printf("\n  arrival accounting\n");
  std::printf("    late         %llu, duplicates %llu, too old %llu\n",
              static_cast<unsigned long long>(queue.late()),
              static_cast<unsigned long long>(queue.duplicates()),
              static_cast<unsigned long long>(queue.too_old()));
  std::printf("    gaps         %llu, discarded %llu, resyncs %llu\n",
              static_cast<unsigned long long>(queue.gaps()),
              static_cast<unsigned long long>(queue.discarded()),
              static_cast<unsigned long long>(queue.resyncs()));

  std::printf("\n  latency, microseconds (%llu frames end to end)\n",
              static_cast<unsigned long long>(stages.total.count()));
  std::printf("    %-22s %8s %8s %8s %8s\n", "stage", "p50", "p95", "p99",
              "max");
  print_stage_row("capture -> encode", stages.encode);
  print_stage_row("queue -> sent", stages.send);
  print_stage_row("sent -> received", stages.wire);
  print_stage_row("network hold", stages.network);
  print_stage_row("jitter buffer", stages.buffer);
  print_stage_row("decode", stages.decode);
  print_stage_row("END TO END", stages.total);

  if (pacer.resyncs() != 0) {
    std::printf(
        "\n  PACER RESYNCS  %llu  (this thread was starved; the run is not a "
        "clean measurement)\n",
        static_cast<unsigned long long>(pacer.resyncs()));
  }
  if (counts.held_overflow != 0) {
    std::printf(
        "  HOLD OVERFLOW  %llu datagrams dropped by the harness, not the "
        "model; raise kHeldCapacity\n",
        static_cast<unsigned long long>(counts.held_overflow));
  }
  if (counts.send_failed != 0 || counts.parse_rejected != 0) {
    std::printf("  send failures  %llu, rejected datagrams %llu\n",
                static_cast<unsigned long long>(counts.send_failed),
                static_cast<unsigned long long>(counts.parse_rejected));
  }

  if (!options.wav_out.empty()) {
    std::printf(
        "\n  output         %s  %.2f s\n", options.wav_out.c_str(),
        static_cast<double>(out_frames) / static_cast<double>(kSampleRateHz));
  }
  if (!options.trace_path.empty()) {
    std::printf("  trace          %s\n", options.trace_path.c_str());
  }
  std::printf("\n");
}

// One stage's histogram as a JSON object. Every stage is emitted the same way
// so the benchmark matrix can build the latency budget by iterating rather than
// by knowing which stages exist -- adding a stage should not mean editing the
// aggregator.
void print_stage_json(const char* name, const Histogram& histogram, bool last) {
  const auto snapshot = histogram.snapshot();
  std::printf(
      R"("%s":{"count":%llu,"p50":%llu,"p95":%llu,"p99":%llu,"max":%llu,)"
      R"("mean":%.1f}%s)",
      name, static_cast<unsigned long long>(snapshot.count),
      static_cast<unsigned long long>(snapshot.p50),
      static_cast<unsigned long long>(snapshot.p95),
      static_cast<unsigned long long>(snapshot.p99),
      static_cast<unsigned long long>(snapshot.max), snapshot.mean,
      last ? "" : ",");
}

void report_json(const Options& options, const Counts& counts,
                 const sim::ImpairmentEngine& net,
                 const audio::JitterBuffer& jitter, const Pacer& pacer,
                 const Stages& stages, std::uint64_t seed) {
  const auto& queue = jitter.queue();

  std::printf(
      R"({"mode":"wavloop","scenario":"%s","seed":%llu,"fec":%s,)"
      R"("expected_loss_percent":%d,"bitrate_bps":%d,"target_delay_ms":%u,)"
      R"("frames_in":%llu,"ticks":%llu,)"
      R"("net":{"considered":%llu,"forwarded":%llu,"dropped":%llu,)"
      R"("dropped_independent":%llu,"dropped_burst":%llu,)"
      R"("duplicated":%llu,"reordered":%llu},)"
      R"("playout":{"from_packet":%llu,"fec_recovered":%llu,"concealed":%llu,)"
      R"("silence":%llu,"muted":%llu},)"
      R"("arrivals":{"late":%llu,"duplicates":%llu,"too_old":%llu,)"
      R"("gaps":%llu,"discarded":%llu,"resyncs":%llu},)",
      options.scenario_name.c_str(), static_cast<unsigned long long>(seed),
      options.fec ? "true" : "false", options.expected_loss_percent,
      options.bitrate_bps, options.target_delay_ms,
      static_cast<unsigned long long>(counts.frames_in),
      static_cast<unsigned long long>(counts.ticks),
      static_cast<unsigned long long>(net.counters().considered),
      static_cast<unsigned long long>(net.counters().forwarded),
      static_cast<unsigned long long>(net.dropped_total()),
      static_cast<unsigned long long>(net.counters().dropped_independent),
      static_cast<unsigned long long>(net.counters().dropped_burst),
      static_cast<unsigned long long>(net.counters().duplicated),
      static_cast<unsigned long long>(net.counters().reordered),
      static_cast<unsigned long long>(jitter.from_packet()),
      static_cast<unsigned long long>(jitter.fec_recovered()),
      static_cast<unsigned long long>(jitter.concealed()),
      static_cast<unsigned long long>(jitter.silence()),
      static_cast<unsigned long long>(jitter.muted()),
      static_cast<unsigned long long>(queue.late()),
      static_cast<unsigned long long>(queue.duplicates()),
      static_cast<unsigned long long>(queue.too_old()),
      static_cast<unsigned long long>(queue.gaps()),
      static_cast<unsigned long long>(queue.discarded()),
      static_cast<unsigned long long>(queue.resyncs()));

  std::printf(R"("stages_us":{)");
  print_stage_json("encode", stages.encode, false);
  print_stage_json("send", stages.send, false);
  print_stage_json("wire", stages.wire, false);
  print_stage_json("network", stages.network, false);
  print_stage_json("buffer", stages.buffer, false);
  print_stage_json("decode", stages.decode, false);
  print_stage_json("total", stages.total, true);
  std::printf("},");

  // The two health flags the matrix refuses a run on. They are last so that a
  // truncated line still fails to parse rather than parsing as a clean run.
  std::printf(R"("pacer_resyncs":%llu,"held_overflow":%llu})"
              "\n",
              static_cast<unsigned long long>(pacer.resyncs()),
              static_cast<unsigned long long>(counts.held_overflow));
}

}  // namespace

int run_wavloop(const Options& options) {
  // ------------------------------------------------------------ the fixture

  auto wav = audio::read_wav(options.wav_in);
  if (!wav.ok()) {
    std::fprintf(stderr, "radiobench: cannot read %s: %s\n",
                 options.wav_in.c_str(), audio::to_string(wav.error));
    const char* hint = audio::conversion_hint(wav.error);
    if (hint != nullptr && hint[0] != '\0') {
      std::fprintf(stderr, "  %s\n", hint);
    }
    return 1;
  }

  // Resampling is deliberately out of scope: it is a signal-processing problem
  // with its own quality tradeoffs, and doing it badly here would show up as a
  // codec result. One ffmpeg command fixes the file instead.
  if (wav.info.sample_rate_hz != kSampleRateHz) {
    std::fprintf(stderr,
                 "radiobench: %s is %u Hz; wavloop needs %u Hz.\n"
                 "  ffmpeg -i %s -ar 48000 -ac 1 -c:a pcm_s16le fixed.wav\n",
                 options.wav_in.c_str(), wav.info.sample_rate_hz, kSampleRateHz,
                 options.wav_in.c_str());
    return 1;
  }

  std::vector<std::int16_t> input =
      wav.info.channels == 1
          ? std::move(wav.samples)
          : audio::downmix_to_mono(wav.samples, wav.info.channels);

  const std::size_t frame_samples = kFrameSamples;
  const Micros frame_us = samples_to_us(frame_samples);

  // The tail of the file is padded rather than dropped, so the last frame of
  // speech is actually encoded and travels the pipeline like any other.
  const std::size_t frames_in =
      (input.size() + frame_samples - 1) / frame_samples;
  input.resize(frames_in * frame_samples, 0);

  if (frames_in == 0) {
    std::fprintf(stderr, "radiobench: %s contains no audio\n",
                 options.wav_in.c_str());
    return 1;
  }

  // ------------------------------------------------------------- the pieces

  audio::EncoderConfig encoder_config;
  encoder_config.bitrate_bps = options.bitrate_bps;
  encoder_config.inband_fec = options.fec;
  encoder_config.expected_loss_percent = options.expected_loss_percent;

  audio::Encoder encoder;
  if (!encoder.open(encoder_config)) {
    std::fprintf(stderr, "radiobench: encoder open failed: %s\n",
                 audio::error_string(encoder.last_error()));
    return 1;
  }

  audio::JitterBuffer::Config jitter_config;
  jitter_config.target_delay_us =
      static_cast<Micros>(options.target_delay_ms) * 1000u;
  jitter_config.fec = options.fec;

  audio::JitterBuffer jitter;
  if (!jitter.open(jitter_config)) {
    std::fprintf(stderr, "radiobench: decoder open failed\n");
    return 1;
  }

  // One seed drives the packet identifiers and the network model together, and
  // it is printed on every run. The scenarios README requires that: a result
  // without its seed cannot be reproduced, and an unreproducible benchmark
  // number is an anecdote.
  const std::uint64_t seed =
      options.seed.has_value() ? *options.seed : std::random_device{}();
  std::mt19937 generator{static_cast<std::mt19937::result_type>(seed)};
  std::uniform_int_distribution<std::uint32_t> any32{0, 0xFFFFFFFFu};
  const std::uint32_t stream_id = any32(generator);
  const std::uint32_t base_sequence = any32(generator);
  const std::uint32_t base_timestamp = any32(generator);

  sim::ImpairmentEngine network(options.impairment, seed);

  net::UdpSocket socket;
  net::UdpSocket::Options socket_options;
  socket_options.family = net::Endpoint::Family::V4;
  socket_options.port = 0;
  // Loopback explicitly, not the wildcard: local_endpoint() has to be an
  // address we can send *to*, and 0.0.0.0 is not one.
  const auto loopback = net::Endpoint::parse("127.0.0.1", 0);
  if (!loopback) {
    std::fprintf(stderr, "radiobench: could not form the loopback address\n");
    return 1;
  }
  socket_options.bind_address = *loopback;

  if (!socket.open(socket_options)) {
    std::fprintf(stderr, "radiobench: could not open socket: %s\n",
                 std::strerror(socket.last_error()));
    return 1;
  }
  const net::Endpoint self = socket.local_endpoint();

  // ------------------------------------------------------------ preallocate

  // How long to keep ticking after the last frame is sent: everything still in
  // flight has to be given its chance to arrive and be played, or the tail of
  // the file would be reported as loss the harness caused.
  const Micros drain_us = jitter_config.target_delay_us +
                          options.impairment.base_delay_us +
                          3 * options.impairment.jitter_us +
                          options.impairment.reorder_extra_us + 5 * frame_us;
  const std::size_t drain_ticks =
      static_cast<std::size_t>(drain_us / frame_us) + 1;
  const std::size_t total_ticks = frames_in + drain_ticks;

  std::vector<TraceRecord> records(frames_in);
  for (std::size_t f = 0; f < frames_in; ++f) {
    records[f].stream_id = stream_id;
    records[f].sequence = base_sequence + static_cast<std::uint32_t>(f);
  }

  std::vector<Held> held(kHeldCapacity);
  std::vector<std::size_t> due;
  due.reserve(kHeldCapacity);

  std::vector<std::int16_t> output;
  output.reserve(total_ticks * frame_samples);

  std::vector<std::int16_t> playout(frame_samples);
  std::vector<std::byte> payload(proto::kMaxPayload);
  std::array<std::byte, proto::kMaxDatagram> datagram{};
  std::array<std::byte, proto::kMaxDatagram> incoming{};

  Stages stages;
  Counts counts;
  counts.frames_in = frames_in;

  // Maps a sequence back to the frame it came from. Out-of-range means a
  // datagram that is not ours, or a sequence the run never produced.
  const auto frame_index = [&](std::uint32_t sequence,
                               std::size_t& out) -> bool {
    const std::int32_t offset = serial::distance(base_sequence, sequence);
    if (offset < 0) return false;
    const auto index = static_cast<std::size_t>(offset);
    if (index >= frames_in) return false;
    out = index;
    return true;
  };

  // ---------------------------------------------------------------- the run

  std::fprintf(
      stderr,
      "radiobench wavloop: %zu frames, %.1f s, scenario '%s', "
      "seed %llu, FEC %s\n",
      frames_in,
      static_cast<double>(frames_in) * static_cast<double>(frame_us) / 1e6,
      options.scenario_name.c_str(), static_cast<unsigned long long>(seed),
      options.fec ? "on" : "off");

  Pacer pacer(frame_us);
  counts.started_us = now_us();
  pacer.start(counts.started_us);

  // Draining is a lambda because it has to happen in two places: once at the
  // top of a tick, and repeatedly *while waiting* for the next one.
  //
  // That second call is not an optimisation. Sleeping through the whole 20 ms
  // and then draining stamps every arrival at a tick boundary, so `sent ->
  // received` reads as a full frame interval and the whole latency budget is
  // 20 ms too large. Measured on macOS before this was split out: 22.9 ms of
  // pure harness. A real receiver has a thread sitting in recvmsg, and the
  // instrument has to match it.
  const auto drain_socket = [&]() {
    for (;;) {
      const auto received = socket.recv_from(incoming);
      if (!received.ok()) return;  // EAGAIN: nothing waiting
      ++counts.datagrams_received;

      const auto parsed =
          proto::parse_media(ByteView{incoming.data(), received.bytes});
      if (!parsed) {
        ++counts.parse_rejected;
        continue;
      }

      std::size_t index = 0;
      if (frame_index(parsed.value.header.sequence, index)) {
        records[index].mark(Stage::Received, received.arrival_us);
      }

      const auto decision = network.decide(received.bytes, received.arrival_us);
      if (!decision.forwarded()) continue;

      // A duplicate draws its own delay, so the copy may arrive before or
      // after the original. Both are stored; the jitter buffer decides which
      // one it keeps.
      const Micros releases[2] = {
          received.arrival_us + decision.delay_us,
          received.arrival_us + decision.duplicate_delay_us};
      const int copies = decision.duplicate ? 2 : 1;

      for (int copy = 0; copy < copies; ++copy) {
        const auto slot =
            std::find_if(held.begin(), held.end(),
                         [](const Held& h) { return !h.occupied; });
        if (slot == held.end()) {
          ++counts.held_overflow;
          break;
        }
        std::copy_n(incoming.data(), received.bytes, slot->bytes.data());
        slot->length = received.bytes;
        slot->release_us = releases[copy];
        slot->occupied = true;
      }
    }
  };

  // How often the socket is checked while waiting. 200 us is 1% of a frame, so
  // it is small against everything being measured, and it is reported rather
  // than assumed: it is the floor under the `sent -> received` column.
  constexpr Micros kPollInterval = 200;

  for (std::size_t tick = 0; tick < total_ticks && !stop_requested(); ++tick) {
    for (;;) {
      const Micros remaining = pacer.delay_until_due_us(now_us());
      if (remaining == 0) break;
      drain_socket();
      const Micros again = pacer.delay_until_due_us(now_us());
      if (again == 0) break;
      sleep_us(std::min<Micros>(again, kPollInterval));
    }
    ++counts.ticks;

    // ---- capture and send ------------------------------------------------
    if (tick < frames_in) {
      TraceRecord& record = records[tick];
      const Micros captured = now_us();
      record.mark(Stage::Captured, captured);

      const std::span<const std::int16_t> frame(
          input.data() + tick * frame_samples, frame_samples);
      const auto encoded = encoder.encode(frame, payload);
      record.mark(Stage::Encoded, now_us());

      if (!encoded.ok()) {
        ++counts.encode_failed;
      } else {
        proto::MediaHeader header;
        header.stream_id = stream_id;
        header.sequence = record.sequence;
        header.timestamp =
            base_timestamp + static_cast<std::uint32_t>(tick) *
                                 static_cast<std::uint32_t>(frame_samples);
        if (tick == 0) header.flags |= proto::media_flag::kTalkspurtStart;
        if (tick + 1 == frames_in) {
          header.flags |= proto::media_flag::kTalkspurtEnd;
        }
        // Advisory per spec section 3.1, and worth setting honestly: a capture
        // of this run should say what the encoder was actually doing.
        if (options.fec) header.flags |= proto::media_flag::kFec;

        record.mark(Stage::Queued, now_us());
        const std::size_t length = proto::encode_media(
            header, ByteView{payload.data(), encoded.bytes}, datagram);
        if (length == 0) {
          ++counts.encode_failed;
        } else {
          const auto sent =
              socket.send_to(self, ByteView{datagram.data(), length});
          record.mark(Stage::Sent, now_us());
          if (!sent.ok()) ++counts.send_failed;
        }
      }
    }

    // ---- receive, and hand each datagram to the network model -------------
    drain_socket();

    // ---- release what the model says has arrived --------------------------
    const Micros now = now_us();
    due.clear();
    for (std::size_t i = 0; i < held.size(); ++i) {
      if (held[i].occupied && held[i].release_us <= now) due.push_back(i);
    }
    // Earliest first. Within one tick the jitter buffer would reorder them
    // anyway, but the trace should say what the model said happened.
    std::sort(due.begin(), due.end(), [&](std::size_t a, std::size_t b) {
      return held[a].release_us < held[b].release_us;
    });

    for (const std::size_t i : due) {
      Held& entry = held[i];
      const auto parsed =
          proto::parse_media(ByteView{entry.bytes.data(), entry.length});
      if (parsed) {
        std::size_t index = 0;
        if (frame_index(parsed.value.header.sequence, index)) {
          records[index].mark(Stage::JitterIn, entry.release_us);
        }
        // The release time, not `now`: that is when the modelled network
        // delivered it, and it is what the buffer must judge lateness against.
        (void)jitter.push(parsed.value, entry.release_us);
      }
      entry.occupied = false;
    }

    // ---- playout ----------------------------------------------------------
    const Micros before_pull = now_us();
    const auto pulled = jitter.pull(playout, before_pull);
    const Micros after_pull = now_us();

    output.insert(output.end(), playout.begin(), playout.end());

    std::size_t index = 0;
    if (frame_index(pulled.sequence, index)) {
      TraceRecord& record = records[index];
      record.mark(Stage::JitterOut, before_pull);
      record.mark(Stage::Decoded, after_pull);
      record.mark(Stage::Played, now_us());
    }

    pacer.advance(now_us());
  }

  counts.finished_us = now_us();

  // ------------------------------------------------------------- the report

  for (const TraceRecord& record : records) {
    record_span(stages.encode, record, Stage::Captured, Stage::Encoded);
    record_span(stages.send, record, Stage::Queued, Stage::Sent);
    record_span(stages.wire, record, Stage::Sent, Stage::Received);
    record_span(stages.network, record, Stage::Received, Stage::JitterIn);
    record_span(stages.buffer, record, Stage::JitterIn, Stage::JitterOut);
    record_span(stages.decode, record, Stage::JitterOut, Stage::Decoded);
    record_span(stages.total, record, Stage::Captured, Stage::Played);
  }

  if (!options.wav_out.empty()) {
    const auto error =
        audio::write_wav(options.wav_out, output, kSampleRateHz, 1);
    if (error != audio::WavError::None) {
      std::fprintf(stderr, "radiobench: cannot write %s: %s\n",
                   options.wav_out.c_str(), audio::to_string(error));
    }
  }

  if (!options.trace_path.empty()) {
    TraceBuffer trace(records.size());
    for (const TraceRecord& record : records) trace.record(record);

    std::FILE* out = std::fopen(options.trace_path.c_str(), "w");
    if (out == nullptr) {
      std::fprintf(stderr, "radiobench: could not open trace file %s: %s\n",
                   options.trace_path.c_str(), std::strerror(errno));
    } else {
      // After the run, never during it: a write() inside the loop would land
      // in the measurement as a latency spike and be read as network jitter.
      if (!trace.write_csv(out)) {
        std::fprintf(stderr, "radiobench: trace write failed\n");
      }
      std::fclose(out);
    }
  }

  if (options.json) {
    report_json(options, counts, network, jitter, pacer, stages, seed);
  } else {
    report_human(options, counts, wav.info, network, jitter, pacer, stages,
                 seed, output.size());
  }

  return stages.total.empty() ? 1 : 0;
}

}  // namespace radiobench

#endif  // RADIO_HAVE_OPUS
