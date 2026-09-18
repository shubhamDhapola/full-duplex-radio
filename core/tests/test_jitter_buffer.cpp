#include "radio/jitter_buffer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "radio/clock.hpp"
#include "radio/opus_codec.hpp"
#include "radio/proto.hpp"

using namespace radio;
using namespace radio::audio;

namespace {

constexpr std::size_t kFrame = kFrameSamples;  // 960 = 20 ms at 48 kHz
constexpr Micros kFrameUs = 20'000;
constexpr Micros kTargetDelay = 60'000;  // three frames

// Arbitrary local-clock origin. Every test drives time explicitly; nothing here
// ever calls now_us(), so the tests are deterministic and take no wall time.
constexpr Micros kEpoch = 1'000'000;

// Deliberately near the wrap, for both sequence and timestamp. The arithmetic
// that has to survive it is the same arithmetic every other test exercises, so
// there is no reason to exercise it anywhere comfortable.
constexpr std::uint32_t kBaseSeq = 0xFFFF'FFC0;
constexpr std::uint32_t kBaseTs = 0xFFFF'F000;

void fill_speechlike(std::vector<std::int16_t>& pcm, double start_seconds) {
  for (std::size_t i = 0; i < pcm.size(); ++i) {
    const double t = start_seconds + static_cast<double>(i) / 48'000.0;
    const double f0 = 120.0 + 60.0 * std::sin(2.0 * M_PI * 1.7 * t);
    double sample = 0.55 * std::sin(2.0 * M_PI * f0 * t) +
                    0.25 * std::sin(2.0 * M_PI * 2.0 * f0 * t + 0.6) +
                    0.12 * std::sin(2.0 * M_PI * 3.0 * f0 * t + 1.1);
    sample *= 0.6 + 0.4 * std::sin(2.0 * M_PI * 3.1 * t);
    pcm[i] = static_cast<std::int16_t>(sample * 12'000.0);
  }
}

// A stream of real Opus packets. Real ones rather than synthetic bytes: the FEC
// path is the whole point of this layer and it cannot be faked -- Opus decides
// for itself whether a packet carries redundancy, and the buffer's counters are
// only meaningful if that decision is the real one.
struct Wire {
  std::vector<std::vector<std::byte>> packets;
  std::uint32_t stream_id = 7;

  [[nodiscard]] std::size_t size() const { return packets.size(); }

  [[nodiscard]] proto::MediaPacket frame(std::size_t f,
                                         std::uint16_t flags = 0) const {
    proto::MediaPacket packet;
    packet.header.flags = flags;
    packet.header.stream_id = stream_id;
    packet.header.sequence = kBaseSeq + static_cast<std::uint32_t>(f);
    packet.header.timestamp =
        kBaseTs + static_cast<std::uint32_t>(f) * kFrameSamples;
    packet.payload = ByteView(packets[f].data(), packets[f].size());
    return packet;
  }

  // Emission time of frame `f` on the sender's clock, and by default its
  // arrival too: a perfect network, which is what the impairment proxy and the
  // tests below perturb deliberately rather than by accident.
  [[nodiscard]] static Micros sent_at(std::size_t f) {
    return kEpoch + static_cast<Micros>(f) * kFrameUs;
  }
};

Wire encode_wire(std::size_t count, bool fec) {
  EncoderConfig config;
  config.inband_fec = fec;
  config.expected_loss_percent = fec ? 20 : 0;
  config.bitrate_bps = 32'000;

  Encoder encoder;
  REQUIRE(encoder.open(config));

  Wire wire;
  std::vector<std::int16_t> pcm(kFrame);
  std::vector<std::byte> buffer(proto::kMaxPayload);
  for (std::size_t f = 0; f < count; ++f) {
    fill_speechlike(pcm, static_cast<double>(f) * 0.02);
    const auto result = encoder.encode(pcm, buffer);
    REQUIRE(result.ok());
    wire.packets.emplace_back(
        buffer.begin(),
        buffer.begin() + static_cast<std::ptrdiff_t>(result.bytes));
  }
  return wire;
}

struct Arrival {
  proto::MediaPacket packet;
  Micros at = 0;
};

struct Transcript {
  std::vector<JitterBuffer::Push> pushes;
  std::vector<JitterBuffer::Pulled> pulls;

  [[nodiscard]] std::size_t count(JitterBuffer::Source source) const {
    return static_cast<std::size_t>(std::count_if(
        pulls.begin(), pulls.end(), [source](const JitterBuffer::Pulled& p) {
          return p.source == source;
        }));
  }
};

// Runs the pipeline the way it actually runs: at every 20 ms tick, whatever the
// network has delivered by then is pushed, and then exactly one frame is
// pulled. Interleaving matters -- a test that pushes everything first and pulls
// afterwards cannot observe a deadline being missed, which is most of what this
// class does.
Transcript drive(JitterBuffer& buffer, std::vector<Arrival> arrivals,
                 std::size_t ticks, Micros start = kEpoch,
                 Micros tick = kFrameUs) {
  std::sort(arrivals.begin(), arrivals.end(),
            [](const Arrival& a, const Arrival& b) { return a.at < b.at; });

  Transcript out;
  std::vector<std::int16_t> pcm(kFrame);
  std::size_t next = 0;

  for (std::size_t k = 0; k < ticks; ++k) {
    const Micros now = start + static_cast<Micros>(k) * tick;
    while (next < arrivals.size() && arrivals[next].at <= now) {
      out.pushes.push_back(
          buffer.push(arrivals[next].packet, arrivals[next].at));
      ++next;
    }
    out.pulls.push_back(buffer.pull(pcm, now));
  }
  return out;
}

// A perfect network: every frame arrives the instant it was sent.
std::vector<Arrival> perfect(const Wire& wire) {
  std::vector<Arrival> arrivals;
  for (std::size_t f = 0; f < wire.size(); ++f) {
    arrivals.push_back({wire.frame(f), Wire::sent_at(f)});
  }
  return arrivals;
}

JitterBuffer::Config default_config() {
  JitterBuffer::Config config;
  config.target_delay_us = kTargetDelay;
  return config;
}

}  // namespace

TEST_CASE("a closed buffer refuses work instead of pretending") {
  JitterBuffer buffer;
  CHECK_FALSE(buffer.is_open());

  const Wire wire = encode_wire(1, false);
  CHECK(buffer.push(wire.frame(0), kEpoch) == JitterBuffer::Push::Closed);

  std::vector<std::int16_t> pcm(kFrame);
  const auto pulled = buffer.pull(pcm, kEpoch);
  CHECK_FALSE(pulled.ok());
  CHECK(pulled.source == JitterBuffer::Source::Silence);
}

TEST_CASE("pull refuses a buffer too small for a frame") {
  JitterBuffer buffer;
  REQUIRE(buffer.open(default_config()));

  std::vector<std::int16_t> pcm(kFrame - 1);
  const auto pulled = buffer.pull(pcm, kEpoch);
  CHECK_FALSE(pulled.ok());
  CHECK(buffer.silence() == 0);  // nothing was produced, so nothing is counted
}

TEST_CASE("the target delay is the pre-roll, and nothing else sets it") {
  const Wire wire = encode_wire(12, false);

  JitterBuffer buffer;
  REQUIRE(buffer.open(default_config()));

  const Transcript run = drive(buffer, perfect(wire), 10);

  // 60 ms of target delay at 20 ms a tick is exactly three ticks of silence,
  // then the stream, in order, from the first frame.
  REQUIRE(run.pulls.size() == 10);
  for (std::size_t k = 0; k < 3; ++k) {
    CHECK(run.pulls[k].source == JitterBuffer::Source::Silence);
  }
  for (std::size_t k = 3; k < 10; ++k) {
    CHECK(run.pulls[k].source == JitterBuffer::Source::Packet);
    CHECK(run.pulls[k].sequence ==
          kBaseSeq + static_cast<std::uint32_t>(k - 3));
    CHECK(run.pulls[k].samples == kFrame);
  }

  CHECK(buffer.silence() == 3);
  CHECK(buffer.from_packet() == 7);
  CHECK(buffer.concealed() == 0);
  CHECK(buffer.anchors() == 1);
  CHECK(buffer.queue().late() == 0);
}

TEST_CASE("a shorter target delay costs less latency and less tolerance") {
  const Wire wire = encode_wire(12, false);

  JitterBuffer::Config config = default_config();
  config.target_delay_us = 20'000;  // one frame

  JitterBuffer buffer;
  REQUIRE(buffer.open(config));

  const Transcript run = drive(buffer, perfect(wire), 6);
  CHECK(run.pulls[0].source == JitterBuffer::Source::Silence);
  CHECK(run.pulls[1].source == JitterBuffer::Source::Packet);
  CHECK(buffer.silence() == 1);
}

TEST_CASE("a swapped pair inside the buffer depth is played in order") {
  const Wire wire = encode_wire(12, false);

  std::vector<Arrival> arrivals = perfect(wire);
  // Frames 4 and 5 trade places on the wire. Both are still comfortably inside
  // the 60 ms the buffer is holding, so neither is late and the listener hears
  // nothing at all.
  std::swap(arrivals[4].at, arrivals[5].at);

  JitterBuffer buffer;
  REQUIRE(buffer.open(default_config()));

  const Transcript run = drive(buffer, arrivals, 10);
  for (std::size_t k = 3; k < 10; ++k) {
    CHECK(run.pulls[k].source == JitterBuffer::Source::Packet);
    CHECK(run.pulls[k].sequence ==
          kBaseSeq + static_cast<std::uint32_t>(k - 3));
  }
  CHECK(buffer.queue().late() == 0);
  CHECK(buffer.concealed() == 0);
}

TEST_CASE("a packet later than the buffer depth is late, not lost") {
  const Wire wire = encode_wire(12, false);

  std::vector<Arrival> arrivals = perfect(wire);
  // 100 ms of delay against a 60 ms buffer: its moment has gone.
  arrivals[4].at += 100'000;

  JitterBuffer buffer;
  REQUIRE(buffer.open(default_config()));

  const Transcript run = drive(buffer, arrivals, 12);

  CHECK(run.pulls[7].source != JitterBuffer::Source::Packet);
  CHECK(run.pulls[7].sequence == kBaseSeq + 4);

  // The distinction spec §6.2 insists on: the network delivered this packet,
  // so it is not loss. Reporting it as loss would send you looking at the
  // radio when the fix is a deeper buffer.
  CHECK(buffer.queue().late() == 1);
  CHECK(buffer.queue().accepted() == 11);
  CHECK(buffer.concealed() == 1);
}

TEST_CASE("a duplicate is counted and played once") {
  const Wire wire = encode_wire(8, false);

  std::vector<Arrival> arrivals = perfect(wire);
  arrivals.push_back({wire.frame(3), Wire::sent_at(3) + 5'000});

  JitterBuffer buffer;
  REQUIRE(buffer.open(default_config()));

  const Transcript run = drive(buffer, arrivals, 9);
  CHECK(buffer.queue().duplicates() == 1);
  CHECK(buffer.from_packet() == 6);
  CHECK(run.pulls[6].sequence == kBaseSeq + 3);
}

TEST_CASE("an isolated loss is recovered from the next packet's redundancy") {
  const Wire wire = encode_wire(14, /*fec=*/true);

  // The encoder decides for itself whether a packet can afford redundancy, so
  // the test asserts that the packet it depends on actually carries it rather
  // than assuming the configuration was enough.
  REQUIRE(Decoder::packet_has_redundancy(
      ByteView(wire.packets[6].data(), wire.packets[6].size())));

  std::vector<Arrival> arrivals = perfect(wire);
  arrivals.erase(arrivals.begin() + 5);  // frame 5 never arrives

  JitterBuffer buffer;
  REQUIRE(buffer.open(default_config()));

  const Transcript run = drive(buffer, arrivals, 12);

  CHECK(run.pulls[8].source == JitterBuffer::Source::ForwardCorrection);
  CHECK(run.pulls[8].sequence == kBaseSeq + 5);
  CHECK(buffer.fec_recovered() == 1);
  CHECK(buffer.concealed() == 0);

  // Frame 6 is still decoded normally on the following tick. Recovering from a
  // packet does not consume it.
  CHECK(run.pulls[9].source == JitterBuffer::Source::Packet);
  CHECK(run.pulls[9].sequence == kBaseSeq + 6);
}

TEST_CASE("the same loss with FEC switched off is concealed instead") {
  const Wire wire = encode_wire(14, /*fec=*/true);

  std::vector<Arrival> arrivals = perfect(wire);
  arrivals.erase(arrivals.begin() + 5);

  JitterBuffer::Config config = default_config();
  config.fec = false;

  JitterBuffer buffer;
  REQUIRE(buffer.open(config));

  const Transcript run = drive(buffer, arrivals, 12);

  // Identical impairment, identical packets, different policy -- which is
  // exactly the comparison M1's acceptance criterion is built on.
  CHECK(run.pulls[8].source == JitterBuffer::Source::Concealment);
  CHECK(buffer.fec_recovered() == 0);
  CHECK(buffer.concealed() == 1);
}

TEST_CASE("a loss whose successor carries no redundancy is concealed") {
  const Wire wire = encode_wire(14, /*fec=*/false);

  std::vector<Arrival> arrivals = perfect(wire);
  arrivals.erase(arrivals.begin() + 5);

  JitterBuffer buffer;
  REQUIRE(buffer.open(default_config()));

  const Transcript run = drive(buffer, arrivals, 12);

  // recover_previous() would have reported success here. The redundancy check
  // is what stops a concealed frame being counted as an FEC recovery, and this
  // is the case that would have inflated the figure.
  CHECK(run.pulls[8].source == JitterBuffer::Source::Concealment);
  CHECK(buffer.fec_recovered() == 0);
  CHECK(buffer.concealed() == 1);
}

TEST_CASE("concealment gives up rather than droning through a dead sender") {
  const Wire wire = encode_wire(4, false);

  JitterBuffer::Config config = default_config();
  config.max_conceal_run = 5;

  JitterBuffer buffer;
  REQUIRE(buffer.open(config));

  // Four frames, then the sender vanishes. 20 ticks is 400 ms of nothing.
  const Transcript run = drive(buffer, perfect(wire), 20);

  CHECK(buffer.from_packet() == 4);
  CHECK(buffer.concealed() == config.max_conceal_run);
  CHECK(buffer.muted() == 1);

  // Ticks 3..6 are the four frames, 7..11 the five concealed, and everything
  // after that is silence rather than an increasingly synthetic drone.
  for (std::size_t k = 7; k < 12; ++k) {
    CHECK(run.pulls[k].source == JitterBuffer::Source::Concealment);
  }
  for (std::size_t k = 12; k < 20; ++k) {
    CHECK(run.pulls[k].source == JitterBuffer::Source::Silence);
  }
}

TEST_CASE("a stalled consumer jumps forward instead of playing stale audio") {
  const Wire wire = encode_wire(40, false);

  JitterBuffer::Config config = default_config();
  config.max_catchup_frames = 5;

  JitterBuffer buffer;
  REQUIRE(buffer.open(config));

  std::vector<std::int16_t> pcm(kFrame);
  const std::vector<Arrival> arrivals = perfect(wire);
  std::size_t next = 0;

  // Six normal ticks: three of pre-roll and three of audio.
  for (std::size_t k = 0; k < 6; ++k) {
    const Micros now = kEpoch + static_cast<Micros>(k) * kFrameUs;
    while (next < arrivals.size() && arrivals[next].at <= now) {
      buffer.push(arrivals[next].packet, arrivals[next].at);
      ++next;
    }
    (void)buffer.pull(pcm, now);
  }
  REQUIRE(buffer.from_packet() == 3);

  // The consumer thread is descheduled for 400 ms. Everything the network sent
  // meanwhile is pushed, then one pull happens far past its deadline.
  const Micros resumed = kEpoch + 6 * kFrameUs + 400'000;
  while (next < arrivals.size() && arrivals[next].at <= resumed) {
    buffer.push(arrivals[next].packet, arrivals[next].at);
    ++next;
  }
  const auto pulled = buffer.pull(pcm, resumed);

  CHECK(buffer.catchups() == 1);
  CHECK(buffer.catchup_frames() == 20);

  // The frame produced is the one due *now*, twenty frames on, not the one the
  // cursor was pointing at when the stall began.
  CHECK(pulled.sequence == kBaseSeq + 23);

  // A catch-up is not packet loss and must not read as packet loss. The frames
  // thrown away had arrived; the queue counts them as discarded.
  CHECK(buffer.queue().discarded() == 20);
  CHECK(buffer.queue().late() == 0);
}

TEST_CASE("a DTX gap inside the buffer depth is silence, not concealment") {
  const Wire wire = encode_wire(10, false);

  std::vector<Arrival> arrivals;
  for (std::size_t f = 0; f < 4; ++f) {
    arrivals.push_back({wire.frame(f), Wire::sent_at(f)});
  }

  // The sender goes quiet for 40 ms and resumes. Sequence stays contiguous
  // (spec §3.3: it counts packets, not time) while the timestamp jumps by the
  // whole silence, and the resumption is flagged.
  //
  // 40 ms and not more, because the buffer's tolerance for this is exactly its
  // own depth: the resumption packet has to reach the queue before the cursor
  // walks over the slot it will occupy, and the cursor reaches that slot
  // target_delay_us after the last frame of the previous talkspurt. A gap
  // longer than 60 ms here is the test below, and it is a different mechanism.
  constexpr std::uint32_t kGapFrames = 2;
  for (std::size_t f = 4; f < 8; ++f) {
    proto::MediaPacket packet = wire.frame(f);
    packet.header.timestamp += kGapFrames * kFrameSamples;
    if (f == 4) packet.header.flags |= proto::media_flag::kTalkspurtStart;
    arrivals.push_back({packet, Wire::sent_at(f) + kGapFrames * kFrameUs});
  }

  JitterBuffer buffer;
  REQUIRE(buffer.open(default_config()));

  // Exactly as many ticks as there is audio for: one more and the test would
  // be measuring what happens after the sender stops, which is the next case.
  const Transcript run = drive(buffer, arrivals, 13);

  CHECK(buffer.from_packet() == 8);
  // Nothing was invented, and nothing was declared lost. The buffer knew the
  // next frame's moment had not come, because the timestamp said so.
  CHECK(buffer.concealed() == 0);
  CHECK(buffer.muted() == 0);
  CHECK(buffer.queue().gaps() == 0);
  CHECK(buffer.anchors() == 2);  // the stream start, and the talkspurt

  // Three ticks of pre-roll, then the gap paid for out of silence.
  CHECK(run.count(JitterBuffer::Source::Silence) == 5);
  for (const auto& pulled : run.pulls) {
    CHECK(pulled.source != JitterBuffer::Source::Concealment);
  }
}

TEST_CASE("a long DTX gap conceals, mutes, and is put back by the talkspurt") {
  const Wire wire = encode_wire(10, false);

  std::vector<Arrival> arrivals;
  for (std::size_t f = 0; f < 4; ++f) {
    arrivals.push_back({wire.frame(f), Wire::sent_at(f)});
  }

  // Two seconds of silence: far longer than the buffer can wait out, so the
  // cursor walks over the gap exactly as it would for a dead sender.
  constexpr std::uint32_t kGapFrames = 100;
  for (std::size_t f = 4; f < 8; ++f) {
    proto::MediaPacket packet = wire.frame(f);
    packet.header.timestamp += kGapFrames * kFrameSamples;
    if (f == 4) packet.header.flags |= proto::media_flag::kTalkspurtStart;
    arrivals.push_back({packet, Wire::sent_at(f) + kGapFrames * kFrameUs});
  }

  JitterBuffer buffer;
  REQUIRE(buffer.open(default_config()));

  // Stops on the tick the last frame of the second talkspurt plays, for the
  // same reason as above.
  const Transcript run = drive(buffer, arrivals, 111);

  // Five concealed frames, then it gives up, and the remaining two seconds
  // cost nothing at all.
  CHECK(buffer.concealed() == 5);
  CHECK(buffer.muted() == 1);
  CHECK(buffer.from_packet() == 8);

  // By the time the resumption arrives the cursor has walked a hundred frames
  // past its sequence, so it comes back as a resync of the timeline rather
  // than an ordinary insert -- and it still plays, on time.
  CHECK(buffer.queue().resyncs() == 1);
  CHECK(buffer.anchors() == 2);
  CHECK(run.count(JitterBuffer::Source::Packet) == 8);
}

TEST_CASE("a sequence far beyond the window resyncs onto itself") {
  const Wire wire = encode_wire(8, false);

  JitterBuffer buffer;
  REQUIRE(buffer.open(default_config()));

  REQUIRE(buffer.push(wire.frame(0), kEpoch) == JitterBuffer::Push::Accepted);

  // The sender restarted: a 1000-frame jump is not reordering under any
  // plausible reading, and waiting for the window to catch up would mean
  // twenty seconds of silence.
  proto::MediaPacket restarted = wire.frame(1);
  restarted.header.sequence = kBaseSeq + 1000;
  restarted.header.timestamp = kBaseTs + 1000 * kFrameSamples;

  CHECK(buffer.push(restarted, kEpoch + kFrameUs) ==
        JitterBuffer::Push::Resynced);
  CHECK(buffer.queue().cursor() == kBaseSeq + 1000);
  CHECK(buffer.queue().resyncs() == 1);
  CHECK(buffer.anchors() == 2);
}

TEST_CASE("a straggler from another stream cannot tear down the live one") {
  const Wire wire = encode_wire(8, false);

  JitterBuffer buffer;
  REQUIRE(buffer.open(default_config()));
  REQUIRE(buffer.push(wire.frame(0), kEpoch) == JitterBuffer::Push::Accepted);

  proto::MediaPacket other = wire.frame(1);
  other.header.stream_id = wire.stream_id + 1;

  CHECK(buffer.push(other, kEpoch + 1'000) == JitterBuffer::Push::Foreign);
  CHECK(buffer.foreign() == 1);
  CHECK(buffer.stream_id() == wire.stream_id);
  CHECK(buffer.anchors() == 1);
}

TEST_CASE("a new stream that announces itself takes over immediately") {
  const Wire wire = encode_wire(8, false);

  JitterBuffer buffer;
  REQUIRE(buffer.open(default_config()));
  REQUIRE(buffer.push(wire.frame(0), kEpoch) == JitterBuffer::Push::Accepted);

  proto::MediaPacket next_talkspurt = wire.frame(1);
  next_talkspurt.header.stream_id = wire.stream_id + 1;
  next_talkspurt.header.flags |= proto::media_flag::kTalkspurtStart;

  CHECK(buffer.push(next_talkspurt, kEpoch + 1'000) ==
        JitterBuffer::Push::Accepted);
  CHECK(buffer.stream_id() == wire.stream_id + 1);
  CHECK(buffer.queue().cursor() == next_talkspurt.header.sequence);
  CHECK(buffer.anchors() == 2);
}

TEST_CASE("an unannounced stream takes over once the old one has gone quiet") {
  const Wire wire = encode_wire(8, false);

  JitterBuffer::Config config = default_config();
  config.stream_idle_us = 200'000;

  JitterBuffer buffer;
  REQUIRE(buffer.open(config));
  REQUIRE(buffer.push(wire.frame(0), kEpoch) == JitterBuffer::Push::Accepted);

  // The packet that would have carried TALKSPURT_START was lost. Without this
  // path the whole talkspurt would be discarded for want of one flag.
  proto::MediaPacket other = wire.frame(1);
  other.header.stream_id = wire.stream_id + 1;

  CHECK(buffer.push(other, kEpoch + 300'000) == JitterBuffer::Push::Accepted);
  CHECK(buffer.stream_id() == wire.stream_id + 1);
  CHECK(buffer.foreign() == 0);
}

TEST_CASE("reset_stream drops the timeline but keeps the session counters") {
  const Wire wire = encode_wire(10, false);

  JitterBuffer buffer;
  REQUIRE(buffer.open(default_config()));
  drive(buffer, perfect(wire), 8);

  const std::uint64_t played = buffer.from_packet();
  REQUIRE(played > 0);

  buffer.reset_stream();
  CHECK_FALSE(buffer.anchored());
  CHECK(buffer.queue().held() == 0);
  CHECK(buffer.from_packet() == played);

  std::vector<std::int16_t> pcm(kFrame);
  const auto pulled = buffer.pull(pcm, kEpoch + 1'000'000);
  CHECK(pulled.source == JitterBuffer::Source::Silence);
}

TEST_CASE("every push and source verdict has a name") {
  using Push = JitterBuffer::Push;
  for (const Push v :
       {Push::Accepted, Push::Duplicate, Push::Late, Push::TooOld,
        Push::Resynced, Push::Foreign, Push::Oversized, Push::Closed}) {
    CHECK(std::string_view(JitterBuffer::to_string(v)) != "unknown");
  }

  using Source = JitterBuffer::Source;
  for (const Source v : {Source::Packet, Source::ForwardCorrection,
                         Source::Concealment, Source::Silence}) {
    CHECK(std::string_view(JitterBuffer::to_string(v)) != "unknown");
  }
}
