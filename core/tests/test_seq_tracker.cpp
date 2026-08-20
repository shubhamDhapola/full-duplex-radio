#include "radio/seq_tracker.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <initializer_list>
#include <string_view>

using namespace radio;
using V = SeqTracker::Verdict;

namespace {
void feed(SeqTracker& t, std::initializer_list<std::uint32_t> seqs) {
  for (const auto s : seqs) t.observe(s);
}
}  // namespace

TEST_CASE("a clean stream reports no loss") {
  SeqTracker t;
  CHECK(t.observe(500) == V::First);
  for (std::uint32_t s = 501; s <= 600; ++s) {
    CHECK(t.observe(s) == V::InOrder);
  }
  CHECK(t.received() == 101);
  CHECK(t.expected() == 101);
  CHECK(t.lost() == 0);
  CHECK(t.duplicates() == 0);
  CHECK(t.reordered() == 0);
  CHECK(t.loss_fraction() == 0.0);
}

TEST_CASE("the worked example is classified correctly") {
  // Sender transmitted 100..109. This is the arrival order:
  //
  //   100 101 103 102 104 104 106 105 109
  //
  //   102 arrived after 103   -> reordered
  //   104 arrived twice       -> duplicate
  //   105 arrived after 106   -> reordered
  //   107, 108 never arrived  -> lost
  SeqTracker t;
  feed(t, {100, 101, 103, 102, 104, 104, 106, 105, 109});

  CHECK(t.expected() == 10);     // span 100..109
  CHECK(t.received() == 8);      // unique arrivals
  CHECK(t.duplicates() == 1);
  CHECK(t.reordered() == 2);
  CHECK(t.lost() == 2);          // 107 and 108

  // The naive "expected - arrivals" formula would have said 1, because the
  // duplicate 104 would have been counted as an arrival and papered over a
  // genuine loss. This is the whole reason the class keeps a bitmap.
}

TEST_CASE("duplicates can never drive the loss count negative") {
  // On unsigned types a negative loss becomes ~1.8e19, which is the classic
  // first bug in a hand-rolled loss counter. Hammer it: one real packet and a
  // hundred copies of it.
  SeqTracker t;
  t.observe(7);
  for (int i = 0; i < 100; ++i) t.observe(7);

  CHECK(t.received() == 1);
  CHECK(t.duplicates() == 100);
  CHECK(t.expected() == 1);
  CHECK(t.lost() == 0);
  CHECK(t.loss_fraction() == 0.0);
}

TEST_CASE("verdicts distinguish reordering from duplication") {
  SeqTracker t;
  CHECK(t.observe(10) == V::First);
  CHECK(t.observe(12) == V::InOrder);    // 11 is now a gap
  CHECK(t.observe(11) == V::Reordered);  // gap filled
  CHECK(t.observe(11) == V::Duplicate);  // already filled
  CHECK(t.observe(12) == V::Duplicate);
  CHECK(t.lost() == 0);
}

TEST_CASE("a reordered packet older than the window is not called a duplicate") {
  // Beyond the window we genuinely cannot know whether we saw it before, and
  // "I don't know" must not be reported as "I know it's a duplicate" — one is
  // evidence about the network, the other is an admission of ignorance.
  constexpr auto kWindow = static_cast<std::uint32_t>(SeqTracker::kWindowSize);
  SeqTracker t;
  t.observe(100'000);
  CHECK(t.observe(100'000u - kWindow) == V::TooOld);
  CHECK(t.too_old() == 1);
  CHECK(t.duplicates() == 0);

  // Just inside the window is still placeable.
  CHECK(t.observe(100'000u - kWindow + 1u) == V::Reordered);
}

TEST_CASE("the circular window does not report phantom duplicates after a lap") {
  // The bug this test exists for: slot(seq) is seq % 1024, so sequence 2 and
  // sequence 1026 share a slot. When the high-water mark jumps over a gap, the
  // slots it crosses must be cleared, or a later reordered arrival lands on a
  // bit set a full lap earlier and is misreported as a duplicate.
  //
  // It cannot reproduce in under a window's worth of traffic, which is why a
  // 20-second smoke test would never find it.
  SeqTracker t;
  t.observe(0);
  for (std::uint32_t s = 1; s <= 1023; ++s) {
    REQUIRE(t.observe(s) == V::InOrder);
  }
  // Every slot 0..1023 is now set.
  REQUIRE(t.received() == 1024);

  // Jump forward past 1024..1029, whose slots are 0..5 — all currently set from
  // sequences 0..5 one lap ago.
  REQUIRE(t.observe(1030) == V::InOrder);

  // Those six are gaps, so a reordered arrival must be accepted, not rejected.
  CHECK(t.observe(1026) == V::Reordered);
  CHECK(t.observe(1024) == V::Reordered);
  CHECK(t.duplicates() == 0);

  // ...and a genuine repeat of one of them is still caught.
  CHECK(t.observe(1026) == V::Duplicate);
}

TEST_CASE("a jump past the whole window invalidates all retained state") {
  SeqTracker t;
  t.observe(0);
  for (std::uint32_t s = 1; s <= 1023; ++s) t.observe(s);

  // Far beyond one lap: nothing remembered can be trusted, so every slot is
  // cleared rather than partially rewound.
  REQUIRE(t.observe(50'000) == V::InOrder);

  // A sequence sharing a slot with an old arrival must not read as a duplicate.
  // 49'000 % 1024 == 872, which was set by sequence 872.
  CHECK(t.observe(49'000) == V::Reordered);
}

TEST_CASE("accounting is correct across the 32-bit wrap") {
  SeqTracker t;
  const std::uint32_t start = 0xFFFFFFFAu;

  CHECK(t.observe(start) == V::First);
  for (std::uint32_t i = 1; i <= 20; ++i) {
    CHECK(t.observe(start + i) == V::InOrder);
  }
  CHECK(t.highest() == 0x0000000Eu);  // wrapped
  CHECK(t.expected() == 21);
  CHECK(t.received() == 21);
  CHECK(t.lost() == 0);
}

TEST_CASE("loss spanning the wrap is counted, not misread as reordering") {
  SeqTracker t;
  t.observe(0xFFFFFFF0u);
  t.observe(0xFFFFFFF1u);
  // 0xFFFFFFF2 .. 0x00000001 lost (16 packets)
  t.observe(0x00000002u);

  CHECK(t.expected() == 19);   // 0xFFFFFFF0 .. 0x00000002 inclusive
  CHECK(t.received() == 3);
  CHECK(t.lost() == 16);
  CHECK(t.reordered() == 0);
}

TEST_CASE("a packet arriving before the first one seen widens the baseline") {
  // We never see the true start of a stream, only the first packet that reaches
  // us. If an earlier one turns up later, the observed span has to grow —
  // otherwise unique arrivals could exceed the expected count.
  SeqTracker t;
  t.observe(1000);
  CHECK(t.expected() == 1);

  CHECK(t.observe(997) == V::Reordered);
  CHECK(t.base() == 997);
  CHECK(t.expected() == 4);    // 997..1000
  CHECK(t.received() == 2);
  CHECK(t.lost() == 2);        // 998, 999
}

TEST_CASE("reset returns the tracker to its initial state") {
  SeqTracker t;
  feed(t, {5, 6, 6, 9});
  REQUIRE(t.received() > 0);

  t.reset();
  CHECK_FALSE(t.started());
  CHECK(t.received() == 0);
  CHECK(t.duplicates() == 0);
  CHECK(t.reordered() == 0);
  CHECK(t.too_old() == 0);
  CHECK(t.expected() == 0);
  CHECK(t.lost() == 0);

  // Reusable afterwards, since streams are per-talkspurt and trackers get
  // recycled.
  CHECK(t.observe(42) == V::First);
}

TEST_CASE("loss fraction matches a hand-computed rate") {
  SeqTracker t;
  // 100 sent, every 10th dropped.
  for (std::uint32_t s = 0; s < 100; ++s) {
    if (s % 10 != 9) t.observe(s);
  }
  CHECK(t.expected() == 99);   // span 0..98, since 99 was dropped
  CHECK(t.received() == 90);
  CHECK(t.lost() == 9);
  CHECK(t.loss_fraction() > 0.09);
  CHECK(t.loss_fraction() < 0.10);
}

TEST_CASE("verdict names are distinct for telemetry") {
  CHECK(std::string_view{to_string(V::First)} == "first");
  CHECK(std::string_view{to_string(V::InOrder)} == "in_order");
  CHECK(std::string_view{to_string(V::Reordered)} == "reordered");
  CHECK(std::string_view{to_string(V::Duplicate)} == "duplicate");
  CHECK(std::string_view{to_string(V::TooOld)} == "too_old");
}
