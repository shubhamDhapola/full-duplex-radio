#include "radio/reorder_queue.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

using namespace radio;
using audio::ReorderQueue;

namespace {

constexpr std::uint32_t kSamplesPerFrame = radio::kFrameSamples;  // 960
constexpr Micros kFrameUs = 20'000;

// Playout timeline used by the tests: frame N is due at N * 20 ms after the
// (arbitrary) epoch. The real mapping lives in the jitter buffer; here it only
// has to be consistent.
constexpr Micros kEpoch = 1'000'000;

ReorderQueue::FrameInfo info_for(std::uint32_t sequence, std::uint32_t base,
                                 std::uint16_t flags = 0) {
  ReorderQueue::FrameInfo info;
  info.header.sequence = sequence;
  info.header.timestamp = (sequence - base) * kSamplesPerFrame;
  info.header.flags = flags;
  info.arrival_us = kEpoch + (sequence - base) * kFrameUs;
  info.deadline_us = kEpoch + (sequence - base) * kFrameUs;
  return info;
}

// A payload whose first byte identifies the frame it belongs to, so a test can
// prove it got the frame it asked for rather than merely a frame.
std::array<std::byte, 8> payload_for(std::uint32_t sequence) {
  std::array<std::byte, 8> bytes{};
  bytes[0] = static_cast<std::byte>(sequence & 0xFF);
  return bytes;
}

ByteView view(const std::array<std::byte, 8>& bytes) {
  return ByteView(bytes.data(), bytes.size());
}

// Inserts frame `sequence` as if it arrived comfortably before its deadline.
ReorderQueue::Insert push(ReorderQueue& q, std::uint32_t sequence,
                          std::uint32_t base) {
  const auto bytes = payload_for(sequence);
  const auto info = info_for(sequence, base);
  return q.insert(info, view(bytes), kEpoch);
}

}  // namespace

TEST_CASE("a fresh queue holds nothing and has no timeline") {
  ReorderQueue q;
  CHECK_FALSE(q.started());
  CHECK(q.held() == 0);
  CHECK(q.depth() == 0);
  CHECK_FALSE(q.has_next());

  ReorderQueue::Frame frame;
  CHECK_FALSE(q.peek_next(frame));
}

TEST_CASE("the first arrival anchors the cursor") {
  ReorderQueue q;
  const std::uint32_t base = 0xFFFF'FFF0;  // deliberately near the wrap

  REQUIRE(push(q, base, base) == ReorderQueue::Insert::Accepted);
  CHECK(q.started());
  CHECK(q.cursor() == base);
  CHECK(q.held() == 1);
  CHECK(q.depth() == 1);
}

TEST_CASE("the cursor anchors wherever the stream is first heard") {
  ReorderQueue q;
  const std::uint32_t base = 1000;

  // Without an explicit start(), the cursor anchors on 1003 because that is
  // what turned up first. The three frames it overtook are then behind the
  // cursor and are refused as late -- which is the truth rather than a
  // shortcoming: by the time they arrived we had already committed to starting
  // playout after them.
  CHECK(push(q, 1003, base) == ReorderQueue::Insert::Accepted);
  CHECK(push(q, 1000, base) == ReorderQueue::Insert::Late);
  CHECK(push(q, 1002, base) == ReorderQueue::Insert::Late);
  CHECK(push(q, 1001, base) == ReorderQueue::Insert::Late);

  CHECK(q.cursor() == 1003);
  CHECK(q.late() == 3);
  CHECK(q.held() == 1);

  // A caller that knows the stream's first sequence some other way -- a control
  // packet, or a previous talkspurt -- calls start() and keeps those frames.
}

TEST_CASE("reordering inside the window is repaired") {
  ReorderQueue q;
  const std::uint32_t base = 1000;
  q.start(base);

  // 1001 overtakes 1000 in flight, which is the whole reason this class exists.
  REQUIRE(push(q, 1001, base) == ReorderQueue::Insert::Accepted);
  REQUIRE(push(q, 1000, base) == ReorderQueue::Insert::Accepted);
  CHECK(q.held() == 2);
  CHECK(q.depth() == 2);

  ReorderQueue::Frame frame;
  REQUIRE(q.peek_next(frame));
  CHECK(frame.info.header.sequence == 1000);
  CHECK(frame.payload[0] == static_cast<std::byte>(1000 & 0xFF));
  CHECK(q.advance() == ReorderQueue::Step::Delivered);

  REQUIRE(q.peek_next(frame));
  CHECK(frame.info.header.sequence == 1001);
  CHECK(q.advance() == ReorderQueue::Step::Delivered);

  CHECK(q.delivered() == 2);
  CHECK(q.gaps() == 0);
  CHECK(q.held() == 0);
  CHECK(q.depth() == 0);
}

TEST_CASE("a missing frame is a gap, and the cursor steps over it") {
  ReorderQueue q;
  const std::uint32_t base = 1000;
  q.start(base);

  REQUIRE(push(q, 1000, base) == ReorderQueue::Insert::Accepted);
  // 1001 never arrives.
  REQUIRE(push(q, 1002, base) == ReorderQueue::Insert::Accepted);

  // depth is 3 while only 2 frames are held: the hole still costs latency.
  CHECK(q.held() == 2);
  CHECK(q.depth() == 3);

  CHECK(q.advance() == ReorderQueue::Step::Delivered);
  CHECK_FALSE(q.has_next());
  CHECK(q.advance() == ReorderQueue::Step::Gap);
  CHECK(q.advance() == ReorderQueue::Step::Delivered);

  CHECK(q.delivered() == 2);
  CHECK(q.gaps() == 1);
}

TEST_CASE("a late arrival is refused rather than played out of place") {
  ReorderQueue q;
  const std::uint32_t base = 1000;
  q.start(base);

  REQUIRE(push(q, 1000, base) == ReorderQueue::Insert::Accepted);
  REQUIRE(q.advance() == ReorderQueue::Step::Delivered);
  REQUIRE(q.advance() == ReorderQueue::Step::Gap);  // concealed 1001

  // 1001 turns up after we already played over it.
  CHECK(push(q, 1001, base) == ReorderQueue::Insert::Late);
  CHECK(q.late() == 1);
  CHECK(q.held() == 0);
}

TEST_CASE("a frame past its deadline is late even while ahead of the cursor") {
  ReorderQueue q;
  const std::uint32_t base = 1000;
  q.start(base);

  const auto bytes = payload_for(1005);
  const auto info = info_for(1005, base);  // due at kEpoch + 100 ms

  // Arrives one microsecond past its moment. Nothing about the cursor says so —
  // this is the check that only the clock can make.
  CHECK(q.insert(info, view(bytes), info.deadline_us + 1) ==
        ReorderQueue::Insert::Late);

  // Arriving exactly on the deadline is still playable.
  CHECK(q.insert(info, view(bytes), info.deadline_us) ==
        ReorderQueue::Insert::Accepted);
  CHECK(q.late() == 1);
  CHECK(q.accepted() == 1);
}

TEST_CASE("duplicates are counted, not stored twice") {
  ReorderQueue q;
  const std::uint32_t base = 1000;
  q.start(base);

  REQUIRE(push(q, 1004, base) == ReorderQueue::Insert::Accepted);
  CHECK(push(q, 1004, base) == ReorderQueue::Insert::Duplicate);
  CHECK(push(q, 1004, base) == ReorderQueue::Insert::Duplicate);
  CHECK(q.duplicates() == 2);
  CHECK(q.held() == 1);
}

TEST_CASE("the window bounds both directions") {
  ReorderQueue q;
  const std::uint32_t base = 1000;
  q.start(base);

  constexpr auto kCap = static_cast<std::uint32_t>(ReorderQueue::kCapacity);

  // The last sequence that still fits.
  CHECK(push(q, base + kCap - 1, base) == ReorderQueue::Insert::Accepted);
  // One past it is not a reorder, it is a stream that moved on without us.
  CHECK(push(q, base + kCap, base) == ReorderQueue::Insert::TooFarAhead);
  CHECK(q.too_far_ahead() == 1);

  // Behind the cursor: late while recent, too old once it is beyond memory.
  CHECK(push(q, base - 1, base) == ReorderQueue::Insert::Late);
  CHECK(push(q, base - kCap + 1, base) == ReorderQueue::Insert::Late);
  CHECK(push(q, base - kCap, base) == ReorderQueue::Insert::TooOld);
  CHECK(q.too_old() == 1);
  CHECK(q.late() == 2);
}

TEST_CASE("sequence comparisons hold across the 32-bit wrap") {
  ReorderQueue q;
  const std::uint32_t base = 0xFFFF'FFFE;
  q.start(base);

  REQUIRE(push(q, 0xFFFF'FFFE, base) == ReorderQueue::Insert::Accepted);
  REQUIRE(push(q, 0xFFFF'FFFF, base) == ReorderQueue::Insert::Accepted);
  REQUIRE(push(q, 0x0000'0000, base) == ReorderQueue::Insert::Accepted);
  REQUIRE(push(q, 0x0000'0001, base) == ReorderQueue::Insert::Accepted);
  CHECK(q.held() == 4);
  CHECK(q.depth() == 4);

  std::vector<std::uint32_t> order;
  ReorderQueue::Frame frame;
  while (q.peek_next(frame)) {
    order.push_back(frame.info.header.sequence);
    REQUIRE(q.advance() == ReorderQueue::Step::Delivered);
  }

  const std::vector<std::uint32_t> expected{0xFFFF'FFFE, 0xFFFF'FFFF,
                                            0x0000'0000, 0x0000'0001};
  CHECK(order == expected);
}

TEST_CASE("a payload too large for a slot is refused, never truncated") {
  ReorderQueue q;
  q.start(0);

  std::vector<std::byte> big(ReorderQueue::kMaxFramePayload + 1);
  ReorderQueue::FrameInfo info;
  info.header.sequence = 0;
  info.deadline_us = kEpoch;

  CHECK(q.insert(info, ByteView(big.data(), big.size()), 0) ==
        ReorderQueue::Insert::Oversized);
  CHECK(q.oversized() == 1);
  CHECK(q.held() == 0);

  // Exactly a slot's worth still fits.
  big.pop_back();
  CHECK(q.insert(info, ByteView(big.data(), big.size()), 0) ==
        ReorderQueue::Insert::Accepted);

  ReorderQueue::Frame frame;
  REQUIRE(q.peek_next(frame));
  CHECK(frame.payload.size() == ReorderQueue::kMaxFramePayload);
}

TEST_CASE("payload and flags survive the queue intact") {
  ReorderQueue q;
  const std::uint32_t base = 77;
  q.start(base);

  const auto bytes = payload_for(base);
  auto info = info_for(
      base, base, proto::media_flag::kTalkspurtStart | proto::media_flag::kFec);
  REQUIRE(q.insert(info, view(bytes), kEpoch) ==
          ReorderQueue::Insert::Accepted);

  ReorderQueue::Frame frame;
  REQUIRE(q.peek_next(frame));
  CHECK(frame.info.header.talkspurt_start());
  CHECK(frame.info.header.fec());
  CHECK(frame.info.header.timestamp == 0);
  CHECK(frame.info.deadline_us == info.deadline_us);
  CHECK(frame.payload.size() == bytes.size());
  CHECK(frame.payload[0] == bytes[0]);
}

TEST_CASE("peek can look ahead of the cursor, which is what FEC needs") {
  ReorderQueue q;
  const std::uint32_t base = 1000;
  q.start(base);

  // 1000 was lost; 1001 arrived carrying redundancy for it.
  REQUIRE(push(q, 1001, base) == ReorderQueue::Insert::Accepted);

  ReorderQueue::Frame frame;
  CHECK_FALSE(q.peek_next(frame));  // the frame we want is not here
  REQUIRE(q.peek(1001, frame));     // but its recovery data is
  CHECK(frame.info.header.sequence == 1001);

  // Outside the window peek refuses rather than returning an aliased slot.
  CHECK_FALSE(q.peek(1001 + ReorderQueue::kCapacity, frame));
  CHECK_FALSE(q.peek(base - 1, frame));
}

TEST_CASE("expired frames are dropped without moving the cursor") {
  ReorderQueue q;
  const std::uint32_t base = 1000;
  q.start(base);

  for (std::uint32_t seq = 1000; seq < 1005; ++seq) {
    REQUIRE(push(q, seq, base) == ReorderQueue::Insert::Accepted);
  }

  // Deadlines are kEpoch, +20 ms, +40 ms ... so this is past the first three.
  CHECK(q.drop_expired(kEpoch + 2 * kFrameUs + 1) == 3);
  CHECK(q.expired() == 3);
  CHECK(q.held() == 2);
  CHECK(q.cursor() == 1000);  // policy is the caller's, not ours

  // Those positions now behave exactly like losses.
  CHECK(q.advance() == ReorderQueue::Step::Gap);
  CHECK(q.advance() == ReorderQueue::Step::Gap);
  CHECK(q.advance() == ReorderQueue::Step::Gap);
  CHECK(q.advance() == ReorderQueue::Step::Delivered);
}

TEST_CASE("skip_to catches up, counting gaps and discarded frames apart") {
  ReorderQueue q;
  const std::uint32_t base = 1000;
  q.start(base);

  REQUIRE(push(q, 1001, base) == ReorderQueue::Insert::Accepted);
  REQUIRE(push(q, 1009, base) == ReorderQueue::Insert::Accepted);

  CHECK(q.skip_to(1009) == 9);
  CHECK(q.cursor() == 1009);
  CHECK(q.gaps() == 9);
  CHECK(q.discarded() == 1);  // 1001 was held and thrown away
  CHECK(q.held() == 1);       // 1009 is still there, and is next

  CHECK(q.has_next());
  CHECK(q.advance() == ReorderQueue::Step::Delivered);

  // It never rewinds.
  CHECK(q.skip_to(1000) == 0);
  CHECK(q.cursor() == 1010);
}

TEST_CASE("skip_to clears every slot when the jump exceeds one window") {
  ReorderQueue q;
  const std::uint32_t base = 1000;
  q.start(base);

  for (std::uint32_t i = 0; i < ReorderQueue::kCapacity; ++i) {
    REQUIRE(push(q, base + i, base) == ReorderQueue::Insert::Accepted);
  }
  REQUIRE(q.held() == ReorderQueue::kCapacity);

  const std::uint32_t target = base + 5000;
  CHECK(q.skip_to(target) == 5000);
  CHECK(q.cursor() == target);
  CHECK(q.held() == 0);
  CHECK(q.discarded() == ReorderQueue::kCapacity);
  CHECK(q.gaps() == 5000);

  // No slot survived to alias against the new cursor position.
  ReorderQueue::Frame frame;
  for (std::uint32_t i = 0; i < ReorderQueue::kCapacity; ++i) {
    CHECK_FALSE(q.peek(target + i, frame));
  }
}

TEST_CASE("resync abandons the stream and re-anchors") {
  ReorderQueue q;
  const std::uint32_t base = 1000;
  q.start(base);
  REQUIRE(push(q, 1000, base) == ReorderQueue::Insert::Accepted);

  // A sender restart lands far outside the window.
  const std::uint32_t restarted = 900'000;
  CHECK(push(q, restarted, base) == ReorderQueue::Insert::TooFarAhead);

  q.resync(restarted);
  CHECK(q.cursor() == restarted);
  CHECK(q.held() == 0);
  CHECK(q.resyncs() == 1);
  CHECK(q.accepted() == 1);  // counters survive a resync

  REQUIRE(push(q, restarted, restarted) == ReorderQueue::Insert::Accepted);
  CHECK(q.advance() == ReorderQueue::Step::Delivered);
}

TEST_CASE("advance before anything arrives is a gap and does not anchor") {
  ReorderQueue q;
  CHECK(q.advance() == ReorderQueue::Step::Gap);
  CHECK(q.gaps() == 1);
  CHECK_FALSE(q.started());

  REQUIRE(push(q, 5000, 5000) == ReorderQueue::Insert::Accepted);
  CHECK(q.cursor() == 5000);
}

TEST_CASE("start does not rewind a running cursor") {
  ReorderQueue q;
  q.start(1000);
  REQUIRE(q.advance() == ReorderQueue::Step::Gap);
  q.start(1);
  CHECK(q.cursor() == 1001);
}

TEST_CASE("reset clears counters, resync keeps them") {
  ReorderQueue q;
  const std::uint32_t base = 1000;
  q.start(base);
  REQUIRE(push(q, base, base) == ReorderQueue::Insert::Accepted);
  REQUIRE(push(q, base, base) == ReorderQueue::Insert::Duplicate);

  q.resync(base + 100);
  CHECK(q.duplicates() == 1);

  q.reset();
  CHECK_FALSE(q.started());
  CHECK(q.accepted() == 0);
  CHECK(q.duplicates() == 0);
  CHECK(q.resyncs() == 0);
  CHECK(q.held() == 0);
}

TEST_CASE("a full window round-trips every frame exactly once") {
  ReorderQueue q;
  const std::uint32_t base = 0xFFFF'FFE0;  // wraps partway through
  q.start(base);

  for (std::uint32_t i = 0; i < ReorderQueue::kCapacity; ++i) {
    REQUIRE(push(q, base + i, base) == ReorderQueue::Insert::Accepted);
  }
  CHECK(q.held() == ReorderQueue::kCapacity);
  CHECK(q.depth() == ReorderQueue::kCapacity);

  ReorderQueue::Frame frame;
  for (std::uint32_t i = 0; i < ReorderQueue::kCapacity; ++i) {
    REQUIRE(q.peek_next(frame));
    CHECK(frame.info.header.sequence == base + i);
    REQUIRE(q.advance() == ReorderQueue::Step::Delivered);
  }
  CHECK(q.held() == 0);
  CHECK(q.delivered() == ReorderQueue::kCapacity);
  CHECK(q.gaps() == 0);
}

TEST_CASE("verdict names are stable, for traces and counters") {
  CHECK(std::string(to_string(ReorderQueue::Insert::Accepted)) == "accepted");
  CHECK(std::string(to_string(ReorderQueue::Insert::TooFarAhead)) ==
        "too_far_ahead");
  CHECK(std::string(to_string(ReorderQueue::Step::Gap)) == "gap");
}
