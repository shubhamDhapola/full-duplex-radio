// Per-packet, per-stage timestamps for the end-to-end latency model.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "radio/clock.hpp"

namespace radio {

// The pipeline stages of docs/latency-model.md. A packet's life, in order.
//
// The first four happen on the sender and are stamped with the sender's clock;
// the last five happen on the receiver, with the receiver's clock. Those two
// groups are NOT directly comparable -- see the note on joining below.
enum class Stage : std::uint8_t {
  Captured = 0,  // microphone frame became available
  Encoded,       // Opus encode returned
  Queued,        // handed to the pacer
  Sent,          // sendto() returned
  Received,      // recvmsg() returned (udp_socket stamps this immediately)
  JitterIn,      // enqueued into the jitter buffer
  JitterOut,     // selected for playout
  Decoded,       // Opus decode returned
  Played,        // handed to the audio device
};

inline constexpr std::size_t kStageCount = 9;

[[nodiscard]] const char* to_string(Stage stage) noexcept;

// WHY THE STAMPS ARE NOT CARRIED IN THE PACKET
//
// The obvious design puts the sender's four timestamps in the packet so the
// receiver can compute end-to-end latency directly. Four uint64 fields is 32
// bytes, which on an 80-byte voice frame is a 40% increase in packet size.
//
// That changes the thing being measured. More bytes means more airtime on a
// contended channel, different queueing behaviour, and a different loss
// profile -- so the instrument would be perturbing its own subject, and the
// measurement would not describe the system as it actually runs.
//
// Instead each side records locally, keyed on (stream_id, sequence), and the
// two halves are joined offline. Zero bytes on the wire and no protocol change.
//
// The cost is that joining across two devices requires the clock offset, which
// carries +/- rtt/2 of uncertainty (see ClockSync). Within one host there is
// one clock, so the join is exact -- which is the concrete reason M1 measures
// the full pipeline on the host before any device work.
struct TraceRecord {
  std::uint32_t stream_id = 0;
  std::uint32_t sequence = 0;

  // 0 means "not recorded". A real monotonic reading is never 0 in practice,
  // and using a sentinel avoids a parallel array of booleans.
  std::array<Micros, kStageCount> at{};

  void mark(Stage stage, Micros when) noexcept {
    at[static_cast<std::size_t>(stage)] = when;
  }

  [[nodiscard]] bool has(Stage stage) const noexcept {
    return at[static_cast<std::size_t>(stage)] != 0;
  }

  [[nodiscard]] Micros when(Stage stage) const noexcept {
    return at[static_cast<std::size_t>(stage)];
  }

  // Elapsed microseconds between two stages, or 0 if either was not recorded.
  //
  // Signed, because a span across the sender/receiver boundary is computed from
  // two different clocks and is legitimately negative until the offset is
  // applied. Returning an unsigned type here would turn that into ~1.8e19, the
  // same trap as in seq_tracker and clock_sync.
  [[nodiscard]] std::int64_t span_us(Stage from, Stage to) const noexcept;
};

// A fixed-capacity ring of trace records.
//
// Capacity is allocated once at construction. record() performs no allocation,
// takes no lock, and does no I/O, because it is called from the packet path and
// eventually from beside an audio callback, where a malloc or a write() is a
// latency spike.
//
// Flushing is a separate, explicitly non-real-time step. That separation is the
// whole point: the hot path appends to memory, and something else turns it into
// a file later.
//
// Single-producer and not thread-safe. Within M0 the tools are single-threaded;
// M1 introduces the lock-free SPSC variant needed once capture and network run
// on separate threads.
class TraceBuffer {
 public:
  explicit TraceBuffer(std::size_t capacity);

  void record(const TraceRecord& record) noexcept;
  void clear() noexcept;

  [[nodiscard]] std::size_t capacity() const noexcept { return slots_.size(); }
  [[nodiscard]] std::size_t size() const noexcept;
  [[nodiscard]] bool empty() const noexcept { return size() == 0; }

  // Records overwritten because the ring wrapped.
  //
  // Non-zero means the trace holds only a recent window rather than the whole
  // run. For a benchmark that is a problem, not a detail -- percentiles over a
  // truncated tail are wrong -- so callers should size the buffer for the run
  // and check this is zero before trusting the numbers.
  [[nodiscard]] std::uint64_t overwritten() const noexcept;

  // Oldest first, so output is in pipeline order.
  [[nodiscard]] const TraceRecord& operator[](std::size_t index) const noexcept;

  // Both write oldest-first and return false on any I/O error. Neither is safe
  // to call from a real-time thread.
  bool write_csv(std::FILE* out) const;
  bool write_jsonl(std::FILE* out) const;

 private:
  std::vector<TraceRecord> slots_;
  std::uint64_t written_ = 0;
};

}  // namespace radio
