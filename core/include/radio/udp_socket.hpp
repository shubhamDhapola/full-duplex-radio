// A UDP socket with an owning handle and a non-blocking receive path.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "radio/bytes.hpp"
#include "radio/clock.hpp"
#include "radio/endpoint.hpp"

namespace radio::net {

// Owns a file descriptor.
//
// The descriptor is closed by the destructor, on every exit path, which is the
// whole point: the alternative is a `close()` call before each of a dozen
// returns and a leak the first time someone adds a thirteenth. A process has a
// descriptor limit, so leaks eventually surface as `socket()` failing in a
// completely unrelated part of the program.
//
// Consequently this type is MOVABLE BUT NOT COPYABLE. Two copies sharing one
// descriptor would both close it; the second close would target a number the
// kernel has since handed to some other file, and the resulting bug -- writes
// going to the wrong place -- is close to undebuggable. Deleting the copy
// operations makes it a compile error instead.
class UdpSocket {
 public:
  struct Options {
    Endpoint::Family family = Endpoint::Family::V4;

    // 0 asks the kernel for an ephemeral port; read it back with
    // local_endpoint().
    std::uint16_t port = 0;

    // Bind to a specific local address. Unspecified binds to the wildcard,
    // which is what a peer that should be reachable on any interface wants.
    Endpoint bind_address{};

    bool reuse_addr = true;

    // Kernel socket buffer sizes.
    //
    // The receive buffer is what holds datagrams between the NIC delivering
    // them and our thread calling recvfrom(). If it fills, the kernel drops
    // packets -- and those drops are indistinguishable from network loss in
    // our metrics, so a too-small buffer looks exactly like a bad network.
    //
    // Voice needs almost nothing (80 bytes x 50/s), but a benchmark flood or a
    // scheduling hiccup can burst hard, and 1 MiB costs nothing on any device
    // we target. Sizing this generously removes a whole class of
    // false-positive loss.
    int recv_buffer_bytes = 1 << 20;
    int send_buffer_bytes = 1 << 20;

    // For a V6 socket, also accept IPv4-mapped traffic, so one socket serves
    // both families.
    bool dual_stack = true;
  };

  struct RecvResult {
    std::size_t bytes = 0;
    Endpoint from{};

    // Captured immediately after recvfrom() returns, before the source address
    // is decoded or anything is parsed.
    //
    // This ordering is not fussiness. Every microsecond between the kernel
    // handing us the datagram and reading the clock is attributed to the
    // network by the jitter estimator. Timestamp after parsing and you are
    // measuring your own parser and calling it jitter -- which is worse than a
    // missing measurement, because it looks plausible.
    Micros arrival_us = 0;

    int error = 0;  // errno; 0 on success

    [[nodiscard]] bool ok() const noexcept { return error == 0; }
    [[nodiscard]] bool would_block() const noexcept;
  };

  struct SendResult {
    std::size_t bytes = 0;
    int error = 0;
    [[nodiscard]] bool ok() const noexcept { return error == 0; }
  };

  UdpSocket() noexcept = default;
  ~UdpSocket();

  UdpSocket(UdpSocket&& other) noexcept;
  UdpSocket& operator=(UdpSocket&& other) noexcept;

  UdpSocket(const UdpSocket&) = delete;
  UdpSocket& operator=(const UdpSocket&) = delete;

  // Two-phase construction rather than construct-or-throw.
  //
  // The whole core is noexcept, because an exception on a real-time path
  // unwinds the stack and may allocate. With no exceptions available a
  // constructor has no way to report failure, so opening is a separate step
  // that returns a status. std::expected would be the modern answer, but it is
  // C++23 and this project targets C++20.
  [[nodiscard]] bool open(const Options& options) noexcept;
  void close() noexcept;

  [[nodiscard]] bool is_open() const noexcept { return fd_ >= 0; }
  [[nodiscard]] int last_error() const noexcept { return last_error_; }

  // The address actually bound, which is how you discover the port the kernel
  // chose when Options::port was 0.
  [[nodiscard]] Endpoint local_endpoint() const noexcept { return local_; }

  // Never blocks: the socket is opened non-blocking. Returns error == EAGAIN
  // when no datagram is waiting.
  [[nodiscard]] RecvResult recv_from(ByteSpan buffer) noexcept;

  [[nodiscard]] SendResult send_to(const Endpoint& to,
                                   ByteView payload) noexcept;

  // Blocks until a datagram is readable or the timeout expires. A negative
  // timeout waits indefinitely. This is the only blocking call in the class,
  // and it belongs to the caller's event loop rather than to the send or
  // receive path.
  [[nodiscard]] bool wait_readable(int timeout_ms) noexcept;

  struct Stats {
    std::uint64_t datagrams_sent = 0;
    std::uint64_t datagrams_received = 0;
    std::uint64_t bytes_sent = 0;
    std::uint64_t bytes_received = 0;
    std::uint64_t send_errors = 0;
    std::uint64_t recv_errors = 0;
    // Datagrams that did not fit the supplied buffer and were truncated. Should
    // always be zero: a non-zero value means a peer is sending packets larger
    // than proto::kMaxDatagram, or our buffer is undersized.
    std::uint64_t truncated = 0;
  };

  [[nodiscard]] const Stats& stats() const noexcept { return stats_; }
  void reset_stats() noexcept { stats_ = Stats{}; }

 private:
  // The multi-socket poll helper below is the only thing outside the class that
  // needs the descriptor, and it needs it purely to hand to poll().
  friend int wait_any_readable(std::span<UdpSocket* const> sockets,
                               std::span<bool> readable,
                               int timeout_ms) noexcept;

  int fd_ = -1;
  int last_error_ = 0;
  Endpoint local_{};
  Stats stats_{};
};

// Waits until at least one of `sockets` is readable, or the timeout expires.
//
// A relay or a multi-peer receive loop needs one poll() covering several
// sockets: polling each in turn with a short timeout would either burn CPU or
// add the timeout to every packet's latency.
//
// This lives in the core rather than in the tool so the file descriptor stays
// private to UdpSocket -- exposing a native_handle() accessor would let any
// caller bypass the class's invariants, and the only thing anyone actually
// needs is this.
//
// `readable` must be the same length as `sockets` and is filled in on return.
// Returns the number readable, 0 on timeout, or -1 on error. At most
// kMaxPolledSockets are accepted.
inline constexpr std::size_t kMaxPolledSockets = 16;

[[nodiscard]] int wait_any_readable(std::span<UdpSocket* const> sockets,
                                    std::span<bool> readable,
                                    int timeout_ms) noexcept;

}  // namespace radio::net
