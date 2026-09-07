#include "radio/pacer.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace radio;

TEST_CASE("an unstarted pacer is never due") {
  Pacer p(20'000);
  CHECK_FALSE(p.started());
  CHECK_FALSE(p.due(1'000'000));
  CHECK(p.delay_until_due_us(1'000'000) == 0);
}

TEST_CASE("a started pacer is due immediately, then one interval later") {
  Pacer p(20'000);
  p.start(1'000'000);
  CHECK(p.started());
  CHECK(p.due(1'000'000));
  CHECK(p.delay_until_due_us(1'000'000) == 0);

  p.advance(1'000'000);
  CHECK(p.next_departure_us() == 1'020'000);
  CHECK_FALSE(p.due(1'010'000));
  CHECK(p.delay_until_due_us(1'010'000) == 10'000);
  CHECK(p.due(1'020'000));
  CHECK(p.departures() == 1);
}

TEST_CASE("lateness on one frame does not accumulate across frames") {
  // The central property. The send thread always wakes 300 us late, which is
  // ordinary scheduling noise. After 100 frames the schedule must still sit on
  // the exact 20 ms grid it started on.
  Pacer p(20'000);
  p.start(1'000'000);

  for (int i = 0; i < 100; ++i) {
    const Micros woke_up_late = p.next_departure_us() + 300;
    p.advance(woke_up_late);
  }

  CHECK(p.next_departure_us() == 1'000'000 + 100 * 20'000);
  CHECK(p.resyncs() == 0);
  CHECK(p.departures() == 100);

  // For contrast, the schedule the obvious `next = now + interval` produces
  // under identical conditions. Each frame's 300 us of lateness is folded in
  // permanently, so the stream ends up 30 ms behind after only two seconds --
  // and the receiver's buffer quietly drains by that amount.
  Micros naive = 1'000'000;
  for (int i = 0; i < 100; ++i) naive = (naive + 300) + 20'000;
  CHECK(naive == 1'000'000 + 100 * 20'000 + 30'000);
}

TEST_CASE("ordinary jitter in wake-up time is tolerated without resyncing") {
  Pacer p(20'000);
  p.start(1'000'000);

  // Alternating early and late arrivals, well inside the catch-up tolerance.
  const Micros offsets[] = {0, 1'500, 15'000, 200, 30'000, 5'000};
  for (const auto offset : offsets) {
    p.advance(p.next_departure_us() + offset);
  }
  CHECK(p.resyncs() == 0);
  CHECK(p.next_departure_us() == 1'000'000 + 6 * 20'000);
}

TEST_CASE("a long stall resynchronises instead of bursting to catch up") {
  // Advancing the schedule blindly would leave ten frames simultaneously due
  // after a 200 ms stall, and sending them back to back is exactly the burst
  // pacing exists to prevent. Those frames are already past any sane playout
  // deadline, so dropping them costs nothing real.
  Pacer p(20'000);
  p.start(1'000'000);
  p.advance(1'000'000);
  REQUIRE(p.next_departure_us() == 1'020'000);

  p.advance(1'220'000);  // thread was descheduled for 200 ms

  CHECK(p.resyncs() == 1);
  CHECK(p.next_departure_us() == 1'220'000);

  // ...and the schedule is clean from there, with no backlog to flush.
  p.advance(1'220'000);
  CHECK(p.next_departure_us() == 1'240'000);
  CHECK(p.resyncs() == 1);
}

TEST_CASE("the catch-up tolerance defaults to two intervals") {
  Pacer p(20'000);
  p.start(0);
  p.advance(0);  // next = 20'000

  // 55 ms late: next becomes 40'000, so we are 15 ms behind -- inside the 40 ms
  // tolerance.
  p.advance(55'000);
  CHECK(p.resyncs() == 0);

  Pacer q(20'000);
  q.start(0);
  q.advance(0);
  // 100 ms late: 60 ms behind the 40'000 schedule, past tolerance.
  q.advance(100'000);
  CHECK(q.resyncs() == 1);
}

TEST_CASE("the tolerance can be set explicitly") {
  // The comparison is made against the schedule *after* it has been advanced:
  // "having already moved on by one interval, am I still further behind than
  // the tolerance allows?" So no resync can happen until a whole slot has been
  // missed, whatever the tolerance is set to.
  Pacer lenient(20'000, 1'000);
  lenient.start(0);
  lenient.advance(0);       // next = 20'000
  lenient.advance(21'500);  // next = 40'000, which is still in the future
  CHECK(lenient.resyncs() == 0);

  Pacer strict(20'000, 1'000);
  strict.start(0);
  strict.advance(0);       // next = 20'000
  strict.advance(45'000);  // next = 40'000, and now is 5 ms past it
  CHECK(strict.resyncs() == 1);
  CHECK(strict.next_departure_us() == 45'000);
}

TEST_CASE("advance on an unstarted pacer starts it") {
  Pacer p(20'000);
  p.advance(500'000);
  CHECK(p.started());
  CHECK(p.next_departure_us() == 520'000);
  CHECK(p.departures() == 1);
}

TEST_CASE("reset clears the schedule but keeps the configured interval") {
  Pacer p(20'000, 5'000);
  p.start(1'000'000);
  p.advance(1'000'000);
  REQUIRE(p.departures() == 1);

  p.reset();
  CHECK_FALSE(p.started());
  CHECK(p.departures() == 0);
  CHECK(p.resyncs() == 0);
  CHECK(p.interval_us() == 20'000);

  // Still configured, so it is reusable for the next talkspurt.
  p.start(2'000'000);
  p.advance(2'000'000);
  CHECK(p.next_departure_us() == 2'020'000);
}
