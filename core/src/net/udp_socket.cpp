#include "radio/udp_socket.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace radio::net {
namespace {

// Converting between Endpoint and the platform's address structs is confined to
// this file, which is what keeps socket headers out of the public interface.
//
// The reinterpret_cast to `sockaddr*` below is the one place this codebase uses
// it. The socket API is defined in terms of a base `sockaddr` that callers pass
// family-specific structs through, and there is no way to express that in
// standard C++ without the cast. Every POSIX program does this; it is blessed by
// practice rather than by the standard. Note the value is built in a correctly
// typed local first and memcpy'd into the storage, so no field is ever *written*
// through a punned pointer.

bool to_sockaddr(const Endpoint& endpoint, sockaddr_storage& out,
                 socklen_t& out_len) noexcept {
  std::memset(&out, 0, sizeof(out));

  switch (endpoint.family()) {
    case Endpoint::Family::V4: {
      sockaddr_in v4{};
      v4.sin_family = AF_INET;
      v4.sin_port = htons(endpoint.port());
      std::memcpy(&v4.sin_addr.s_addr, endpoint.address().data(), 4);
      std::memcpy(&out, &v4, sizeof(v4));
      out_len = sizeof(v4);
      return true;
    }
    case Endpoint::Family::V6: {
      sockaddr_in6 v6{};
      v6.sin6_family = AF_INET6;
      v6.sin6_port = htons(endpoint.port());
      v6.sin6_scope_id = endpoint.scope_id();
      std::memcpy(&v6.sin6_addr.s6_addr, endpoint.address().data(), 16);
      std::memcpy(&out, &v6, sizeof(v6));
      out_len = sizeof(v6);
      return true;
    }
    case Endpoint::Family::Unspecified:
      break;
  }
  return false;
}

Endpoint from_sockaddr(const sockaddr_storage& storage) noexcept {
  if (storage.ss_family == AF_INET) {
    sockaddr_in v4{};
    std::memcpy(&v4, &storage, sizeof(v4));
    std::array<std::byte, 4> bytes{};
    std::memcpy(bytes.data(), &v4.sin_addr.s_addr, 4);
    return Endpoint::v4(bytes, ntohs(v4.sin_port));
  }
  if (storage.ss_family == AF_INET6) {
    sockaddr_in6 v6{};
    std::memcpy(&v6, &storage, sizeof(v6));
    std::array<std::byte, 16> bytes{};
    std::memcpy(bytes.data(), &v6.sin6_addr.s6_addr, 16);
    return Endpoint::v6(bytes, ntohs(v6.sin6_port), v6.sin6_scope_id);
  }
  return {};
}

int family_to_af(Endpoint::Family family) noexcept {
  return family == Endpoint::Family::V6 ? AF_INET6 : AF_INET;
}

}  // namespace

bool UdpSocket::RecvResult::would_block() const noexcept {
  return error == EAGAIN || error == EWOULDBLOCK;
}

UdpSocket::~UdpSocket() { close(); }

UdpSocket::UdpSocket(UdpSocket&& other) noexcept
    : fd_(other.fd_),
      last_error_(other.last_error_),
      local_(other.local_),
      stats_(other.stats_) {
  // Critical: the moved-from object must no longer own the descriptor, or both
  // destructors will close it.
  other.fd_ = -1;
}

UdpSocket& UdpSocket::operator=(UdpSocket&& other) noexcept {
  if (this != &other) {
    close();
    fd_ = other.fd_;
    last_error_ = other.last_error_;
    local_ = other.local_;
    stats_ = other.stats_;
    other.fd_ = -1;
  }
  return *this;
}

void UdpSocket::close() noexcept {
  if (fd_ >= 0) {
    // close() is not retried on EINTR: on Linux and macOS the descriptor is
    // released regardless, and retrying risks closing a descriptor another
    // thread has since been handed.
    ::close(fd_);
    fd_ = -1;
  }
}

bool UdpSocket::open(const Options& options) noexcept {
  close();

  const int af = family_to_af(options.family);
  fd_ = ::socket(af, SOCK_DGRAM, IPPROTO_UDP);
  if (fd_ < 0) {
    last_error_ = errno;
    return false;
  }

  const auto fail = [this]() noexcept {
    last_error_ = errno;
    close();
    return false;
  };

  if (options.reuse_addr) {
    int on = 1;
    if (::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0) {
      return fail();
    }
  }

  if (af == AF_INET6) {
    // 0 means "also accept IPv4-mapped addresses", so a single socket serves
    // both families and there is one bind, one firewall rule, one discovery
    // record.
    int v6only = options.dual_stack ? 0 : 1;
    if (::setsockopt(fd_, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only)) <
        0) {
      return fail();
    }
  }

  // Buffer sizing is a request, not a guarantee: the kernel may clamp it, and
  // it silently halves or doubles the value on some platforms. A failure here
  // is not fatal -- the socket still works with the default -- so it is
  // recorded rather than treated as an error.
  if (options.recv_buffer_bytes > 0) {
    int size = options.recv_buffer_bytes;
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
  }
  if (options.send_buffer_bytes > 0) {
    int size = options.send_buffer_bytes;
    ::setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
  }

  // Non-blocking from the outset. A blocking recvfrom() would park the thread
  // indefinitely, so shutdown would need a self-pipe or a signal to break out;
  // with a non-blocking socket the receive loop polls with a timeout and can
  // check a stop flag on every pass.
  //
  // SOCK_NONBLOCK on socket() would be one syscall instead of three, but it is
  // Linux-only, and this has to build on macOS too.
  const int flags = ::fcntl(fd_, F_GETFL, 0);
  if (flags < 0 || ::fcntl(fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
    return fail();
  }

  sockaddr_storage bind_storage{};
  socklen_t bind_len = 0;

  if (options.bind_address.valid()) {
    if (!to_sockaddr(options.bind_address, bind_storage, bind_len)) {
      errno = EINVAL;
      return fail();
    }
  } else if (af == AF_INET) {
    sockaddr_in v4{};
    v4.sin_family = AF_INET;
    v4.sin_port = htons(options.port);
    v4.sin_addr.s_addr = htonl(INADDR_ANY);
    std::memcpy(&bind_storage, &v4, sizeof(v4));
    bind_len = sizeof(v4);
  } else {
    sockaddr_in6 v6{};
    v6.sin6_family = AF_INET6;
    v6.sin6_port = htons(options.port);
    v6.sin6_addr = in6addr_any;
    std::memcpy(&bind_storage, &v6, sizeof(v6));
    bind_len = sizeof(v6);
  }

  if (::bind(fd_, reinterpret_cast<const sockaddr*>(&bind_storage), bind_len) <
      0) {
    return fail();
  }

  // Read back what we actually got. When Options::port was 0 the kernel chose
  // an ephemeral port, and this is the only way to learn it.
  sockaddr_storage actual{};
  socklen_t actual_len = sizeof(actual);
  if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&actual), &actual_len) ==
      0) {
    local_ = from_sockaddr(actual);
  }

  last_error_ = 0;
  return true;
}

UdpSocket::RecvResult UdpSocket::recv_from(ByteSpan buffer) noexcept {
  RecvResult result;

  if (fd_ < 0) {
    result.error = EBADF;
    return result;
  }

  sockaddr_storage from{};
  iovec iov{};
  iov.iov_base = buffer.data();
  iov.iov_len = buffer.size();

  msghdr message{};
  message.msg_name = &from;
  message.msg_namelen = sizeof(from);
  message.msg_iov = &iov;
  message.msg_iovlen = 1;

  // recvmsg rather than recvfrom, purely so msg_flags can be checked for
  // MSG_TRUNC. recvfrom silently discards the tail of an oversized datagram,
  // which would show up much later as a stream of malformed-packet counters
  // with no indication of the real cause.
  ssize_t received = 0;
  do {
    received = ::recvmsg(fd_, &message, 0);
    // EINTR means a signal arrived mid-syscall and nothing was read. Retrying
    // is correct; treating it as an error would make the receive loop drop
    // packets whenever a profiler or debugger attached.
  } while (received < 0 && errno == EINTR);

  // Immediately. Anything between the syscall returning and this line is
  // measured as network jitter by the estimator.
  result.arrival_us = now_us();

  if (received < 0) {
    result.error = errno;
    if (!result.would_block()) ++stats_.recv_errors;
    return result;
  }

  result.bytes = static_cast<std::size_t>(received);
  result.from = from_sockaddr(from);

  if ((message.msg_flags & MSG_TRUNC) != 0) {
    ++stats_.truncated;
  }

  ++stats_.datagrams_received;
  stats_.bytes_received += result.bytes;
  return result;
}

UdpSocket::SendResult UdpSocket::send_to(const Endpoint& to,
                                         ByteView payload) noexcept {
  SendResult result;

  if (fd_ < 0) {
    result.error = EBADF;
    return result;
  }

  sockaddr_storage storage{};
  socklen_t length = 0;
  if (!to_sockaddr(to, storage, length)) {
    result.error = EINVAL;
    ++stats_.send_errors;
    return result;
  }

  ssize_t sent = 0;
  do {
    sent = ::sendto(fd_, payload.data(), payload.size(), 0,
                    reinterpret_cast<const sockaddr*>(&storage), length);
  } while (sent < 0 && errno == EINTR);

  if (sent < 0) {
    result.error = errno;
    ++stats_.send_errors;
    return result;
  }

  result.bytes = static_cast<std::size_t>(sent);
  ++stats_.datagrams_sent;
  stats_.bytes_sent += result.bytes;
  return result;
}

bool UdpSocket::wait_readable(int timeout_ms) noexcept {
  if (fd_ < 0) return false;

  pollfd descriptor{};
  descriptor.fd = fd_;
  descriptor.events = POLLIN;

  int ready = 0;
  do {
    ready = ::poll(&descriptor, 1, timeout_ms);
  } while (ready < 0 && errno == EINTR);

  if (ready < 0) {
    last_error_ = errno;
    return false;
  }
  return ready > 0 && (descriptor.revents & POLLIN) != 0;
}

}  // namespace radio::net
