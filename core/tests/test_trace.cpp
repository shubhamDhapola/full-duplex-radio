#include "radio/trace.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <string>
#include <string_view>

using namespace radio;

namespace {

// Renders a buffer through the given writer into a string, so the output format
// can be asserted without touching the filesystem.
template <class Writer>
std::string render(const TraceBuffer& buffer, Writer writer) {
  std::string out;
  char* data = nullptr;
  std::size_t length = 0;
  std::FILE* stream = ::open_memstream(&data, &length);
  REQUIRE(stream != nullptr);
  REQUIRE(writer(buffer, stream));
  std::fclose(stream);
  out.assign(data, length);
  std::free(data);
  return out;
}

std::string to_csv(const TraceBuffer& b) {
  return render(b, [](const TraceBuffer& x, std::FILE* f) { return x.write_csv(f); });
}
std::string to_jsonl(const TraceBuffer& b) {
  return render(b, [](const TraceBuffer& x, std::FILE* f) { return x.write_jsonl(f); });
}

TraceRecord make(std::uint32_t sequence, Micros sent, Micros received) {
  TraceRecord r;
  r.stream_id = 0xABCD;
  r.sequence = sequence;
  r.mark(Stage::Sent, sent);
  r.mark(Stage::Received, received);
  return r;
}

}  // namespace

TEST_CASE("an unmarked stage reads as absent rather than as time zero") {
  TraceRecord r;
  for (std::size_t s = 0; s < kStageCount; ++s) {
    CHECK_FALSE(r.has(static_cast<Stage>(s)));
  }
  r.mark(Stage::Encoded, 5'000);
  CHECK(r.has(Stage::Encoded));
  CHECK(r.when(Stage::Encoded) == 5'000);
  CHECK_FALSE(r.has(Stage::Decoded));
}

TEST_CASE("a span between recorded stages is the elapsed time") {
  TraceRecord r;
  r.mark(Stage::Captured, 1'000'000);
  r.mark(Stage::Encoded, 1'001'800);
  r.mark(Stage::Sent, 1'002'100);

  CHECK(r.span_us(Stage::Captured, Stage::Encoded) == 1'800);
  CHECK(r.span_us(Stage::Encoded, Stage::Sent) == 300);
  CHECK(r.span_us(Stage::Captured, Stage::Sent) == 2'100);
}

TEST_CASE("a span involving an unrecorded stage is zero, not garbage") {
  TraceRecord r;
  r.mark(Stage::Captured, 1'000'000);
  CHECK(r.span_us(Stage::Captured, Stage::Played) == 0);
  CHECK(r.span_us(Stage::Queued, Stage::Sent) == 0);
}

TEST_CASE("a span is signed so a cross-clock comparison stays readable") {
  // Sender stages carry the sender's clock and receiver stages the receiver's,
  // and the two epochs are unrelated. Before the offset is applied a span
  // across that boundary can be negative -- which must read as a negative
  // number rather than as 1.8e19.
  TraceRecord r;
  r.mark(Stage::Sent, 9'000'000);      // sender clock, high epoch
  r.mark(Stage::Received, 1'000'000);  // receiver clock, low epoch
  CHECK(r.span_us(Stage::Sent, Stage::Received) == -8'000'000);
}

TEST_CASE("a buffer under capacity keeps everything, oldest first") {
  TraceBuffer buffer(16);
  CHECK(buffer.empty());
  CHECK(buffer.capacity() == 16);

  for (std::uint32_t i = 0; i < 5; ++i) {
    buffer.record(make(100 + i, 1'000 + i, 2'000 + i));
  }

  CHECK(buffer.size() == 5);
  CHECK(buffer.overwritten() == 0);
  CHECK(buffer[0].sequence == 100);
  CHECK(buffer[4].sequence == 104);
}

TEST_CASE("a full buffer overwrites the oldest and says so") {
  // A silently truncated trace produces percentiles over a partial tail, which
  // are wrong in a way nothing downstream can detect. So the condition is
  // counted and callers are expected to check it.
  TraceBuffer buffer(4);
  for (std::uint32_t i = 0; i < 10; ++i) {
    buffer.record(make(i, 1'000 + i, 2'000 + i));
  }

  CHECK(buffer.size() == 4);
  CHECK(buffer.overwritten() == 6);

  // The four surviving records are the most recent, still oldest-first.
  CHECK(buffer[0].sequence == 6);
  CHECK(buffer[1].sequence == 7);
  CHECK(buffer[2].sequence == 8);
  CHECK(buffer[3].sequence == 9);
}

TEST_CASE("wrapping exactly once leaves the order correct") {
  // The off-by-one-lap case, same shape as the SeqTracker window bug in
  // lesson 05: the read cursor has to follow the write cursor round.
  TraceBuffer buffer(4);
  for (std::uint32_t i = 0; i < 4; ++i) buffer.record(make(i, i, i));
  CHECK(buffer[0].sequence == 0);

  buffer.record(make(99, 99, 99));
  CHECK(buffer.overwritten() == 1);
  CHECK(buffer[0].sequence == 1);
  CHECK(buffer[3].sequence == 99);
}

TEST_CASE("a zero-capacity buffer is still usable") {
  // Guarding at construction rather than making every record() check.
  TraceBuffer buffer(0);
  CHECK(buffer.capacity() == 1);
  buffer.record(make(7, 1, 2));
  CHECK(buffer.size() == 1);
  CHECK(buffer[0].sequence == 7);
}

TEST_CASE("clear empties the buffer for reuse") {
  TraceBuffer buffer(8);
  for (std::uint32_t i = 0; i < 8; ++i) buffer.record(make(i, i, i));
  buffer.clear();
  CHECK(buffer.empty());
  CHECK(buffer.overwritten() == 0);

  buffer.record(make(42, 1, 2));
  CHECK(buffer.size() == 1);
  CHECK(buffer[0].sequence == 42);
}

TEST_CASE("CSV output has a header naming every stage") {
  TraceBuffer buffer(4);
  buffer.record(make(1, 1'000, 2'000));
  const auto csv = to_csv(buffer);

  CHECK(csv.starts_with("stream_id,sequence,captured,encoded,queued,sent,"));
  CHECK(csv.find("received,jitter_in,jitter_out,decoded,played\n") !=
        std::string::npos);
}

TEST_CASE("CSV leaves unrecorded stages empty rather than writing zero") {
  // A plotting script must not be able to confuse "this stage never happened"
  // with "this stage happened at time zero".
  TraceBuffer buffer(4);
  buffer.record(make(1, 1'000, 2'000));
  const auto csv = to_csv(buffer);

  // stream_id, sequence, then captured/encoded/queued absent, sent, received,
  // then four absent.
  CHECK(csv.find("43981,1,,,,1000,2000,,,,\n") != std::string::npos);
  CHECK(csv.find(",0,") == std::string::npos);
}

TEST_CASE("JSONL emits one object per record and omits absent stages") {
  TraceBuffer buffer(4);
  buffer.record(make(1, 1'000, 2'000));
  buffer.record(make(2, 1'020, 2'020));
  const auto jsonl = to_jsonl(buffer);

  CHECK(jsonl ==
        "{\"stream_id\":43981,\"sequence\":1,\"sent\":1000,\"received\":2000}\n"
        "{\"stream_id\":43981,\"sequence\":2,\"sent\":1020,\"received\":2020}\n");

  // Omission, not null: a consumer checks for the key's presence.
  CHECK(jsonl.find("null") == std::string::npos);
  CHECK(jsonl.find("captured") == std::string::npos);
}

TEST_CASE("an empty buffer produces no JSONL rows and a bare CSV header") {
  TraceBuffer buffer(4);
  CHECK(to_jsonl(buffer).empty());
  const auto csv = to_csv(buffer);
  CHECK(csv.find('\n') == csv.size() - 1);  // header only
}

TEST_CASE("a full pipeline record yields the whole latency breakdown") {
  // The docs/latency-model.md table, computed from one record.
  TraceRecord r;
  r.stream_id = 1;
  r.sequence = 500;
  Micros t = 10'000'000;
  r.mark(Stage::Captured, t);
  r.mark(Stage::Encoded, t += 1'800);
  r.mark(Stage::Queued, t += 200);
  r.mark(Stage::Sent, t += 900);
  r.mark(Stage::Received, t += 4'700);
  r.mark(Stage::JitterIn, t += 100);
  r.mark(Stage::JitterOut, t += 24'500);
  r.mark(Stage::Decoded, t += 700);
  r.mark(Stage::Played, t += 11'800);

  CHECK(r.span_us(Stage::Captured, Stage::Encoded) == 1'800);
  CHECK(r.span_us(Stage::Sent, Stage::Received) == 4'700);
  CHECK(r.span_us(Stage::JitterIn, Stage::JitterOut) == 24'500);
  CHECK(r.span_us(Stage::Captured, Stage::Played) == 44'700);

  // The jitter buffer is the single largest contributor, which is the whole
  // reason adaptive buffering (M4) is worth the effort: it is the one term big
  // enough that halving it is audible.
  const auto total = r.span_us(Stage::Captured, Stage::Played);
  const auto in_buffer = r.span_us(Stage::JitterIn, Stage::JitterOut);
  CHECK(in_buffer * 2 > total);
}
