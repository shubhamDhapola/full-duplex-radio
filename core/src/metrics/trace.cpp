#include "radio/trace.hpp"

namespace radio {

const char* to_string(Stage stage) noexcept {
  switch (stage) {
    case Stage::Captured:
      return "captured";
    case Stage::Encoded:
      return "encoded";
    case Stage::Queued:
      return "queued";
    case Stage::Sent:
      return "sent";
    case Stage::Received:
      return "received";
    case Stage::JitterIn:
      return "jitter_in";
    case Stage::JitterOut:
      return "jitter_out";
    case Stage::Decoded:
      return "decoded";
    case Stage::Played:
      return "played";
  }
  return "invalid";
}

std::int64_t TraceRecord::span_us(Stage from, Stage to) const noexcept {
  if (!has(from) || !has(to)) return 0;
  return static_cast<std::int64_t>(when(to)) -
         static_cast<std::int64_t>(when(from));
}

TraceBuffer::TraceBuffer(std::size_t capacity)
    : slots_(capacity == 0 ? 1 : capacity) {}

void TraceBuffer::record(const TraceRecord& record) noexcept {
  // The only work on the hot path: one modulo and one struct copy into
  // already-allocated storage.
  slots_[written_ % slots_.size()] = record;
  ++written_;
}

void TraceBuffer::clear() noexcept { written_ = 0; }

std::size_t TraceBuffer::size() const noexcept {
  return written_ < slots_.size() ? static_cast<std::size_t>(written_)
                                  : slots_.size();
}

std::uint64_t TraceBuffer::overwritten() const noexcept {
  return written_ > slots_.size() ? written_ - slots_.size() : 0;
}

const TraceRecord& TraceBuffer::operator[](std::size_t index) const noexcept {
  // Once the ring has wrapped, the oldest surviving record sits just after the
  // write cursor. Presenting them oldest-first keeps output in pipeline order,
  // which is what makes a trace readable.
  const std::size_t start =
      written_ < slots_.size() ? 0 : static_cast<std::size_t>(written_ % slots_.size());
  return slots_[(start + index) % slots_.size()];
}

bool TraceBuffer::write_csv(std::FILE* out) const {
  if (std::fprintf(out, "stream_id,sequence") < 0) return false;
  for (std::size_t s = 0; s < kStageCount; ++s) {
    if (std::fprintf(out, ",%s", to_string(static_cast<Stage>(s))) < 0) {
      return false;
    }
  }
  if (std::fprintf(out, "\n") < 0) return false;

  const std::size_t count = size();
  for (std::size_t i = 0; i < count; ++i) {
    const auto& record = (*this)[i];
    if (std::fprintf(out, "%u,%u", record.stream_id, record.sequence) < 0) {
      return false;
    }
    for (std::size_t s = 0; s < kStageCount; ++s) {
      // An unrecorded stage is left as an empty field rather than written as 0,
      // so a plotting script cannot mistake "never happened" for "happened at
      // time zero".
      if (record.at[s] == 0) {
        if (std::fprintf(out, ",") < 0) return false;
      } else if (std::fprintf(out, ",%llu",
                              static_cast<unsigned long long>(record.at[s])) <
                 0) {
        return false;
      }
    }
    if (std::fprintf(out, "\n") < 0) return false;
  }
  return true;
}

bool TraceBuffer::write_jsonl(std::FILE* out) const {
  const std::size_t count = size();
  for (std::size_t i = 0; i < count; ++i) {
    const auto& record = (*this)[i];
    if (std::fprintf(out, R"({"stream_id":%u,"sequence":%u)", record.stream_id,
                     record.sequence) < 0) {
      return false;
    }
    for (std::size_t s = 0; s < kStageCount; ++s) {
      if (record.at[s] == 0) continue;  // omit rather than emit null
      if (std::fprintf(out, R"(,"%s":%llu)", to_string(static_cast<Stage>(s)),
                       static_cast<unsigned long long>(record.at[s])) < 0) {
        return false;
      }
    }
    if (std::fprintf(out, "}\n") < 0) return false;
  }
  return true;
}

}  // namespace radio
