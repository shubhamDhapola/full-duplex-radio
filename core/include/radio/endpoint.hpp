// An IP address and port, without leaking socket headers into the interface.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace radio::net {

// Deliberately does NOT store a `sockaddr_storage`.
//
// The obvious implementation wraps the platform's address struct directly. That
// would drag <sys/socket.h> and <netinet/in.h> into every file that mentions an
// endpoint -- including the JNI bridge and, later, the Swift-facing C ABI --
// and it would make the type awkward to compare, copy or use as a hash key,
// because `sockaddr_storage` is 128 bytes of mostly padding whose unused tail
// is not guaranteed to be zeroed.
//
// Instead the address is held as plain bytes and converted to a `sockaddr` only
// inside udp_socket.cpp, at the syscall boundary. The type then becomes
// trivially copyable, comparable, and cheap to pass around -- which matters
// because a peer table keyed on endpoints is coming in M3.
class Endpoint {
 public:
  enum class Family : std::uint8_t { Unspecified, V4, V6 };

  Endpoint() = default;

  [[nodiscard]] static Endpoint v4(std::array<std::byte, 4> address,
                                   std::uint16_t port) noexcept;
  [[nodiscard]] static Endpoint v6(std::array<std::byte, 16> address,
                                   std::uint16_t port,
                                   std::uint32_t scope_id = 0) noexcept;

  // Parses a numeric address only -- "192.168.1.14" or "fe80::1", never
  // "myphone.local".
  //
  // Refusing hostnames is a design choice, not a limitation. Name resolution
  // blocks: getaddrinfo() can sit on the network for seconds, and a blocking
  // call anywhere reachable from the media path is a latency spike waiting to
  // happen. Peer discovery is mDNS's job (M3), and it hands us numeric
  // addresses. Keeping this function incapable of blocking means it can never
  // become the cause of one.
  [[nodiscard]] static std::optional<Endpoint> parse(std::string_view address,
                                                     std::uint16_t port) noexcept;

  // Accepts "host:port", with IPv6 in brackets: "[fe80::1]:47000".
  [[nodiscard]] static std::optional<Endpoint> parse_with_port(
      std::string_view text) noexcept;

  [[nodiscard]] Family family() const noexcept { return family_; }
  [[nodiscard]] bool valid() const noexcept {
    return family_ != Family::Unspecified;
  }
  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
  [[nodiscard]] std::uint32_t scope_id() const noexcept { return scope_id_; }

  // 4 bytes for V4, 16 for V6, empty when unspecified.
  [[nodiscard]] std::span<const std::byte> address() const noexcept;

  [[nodiscard]] std::string to_string() const;

  // C++20 lets the compiler write this from the members, which is both shorter
  // and safer than a hand-rolled version -- adding a member later can't leave a
  // stale comparison behind. The address array is fully zero-initialised, so a
  // V4 endpoint's unused 12 tail bytes never make two equal addresses compare
  // unequal.
  [[nodiscard]] bool operator==(const Endpoint&) const noexcept = default;

 private:
  std::array<std::byte, 16> address_{};
  std::uint32_t scope_id_ = 0;
  std::uint16_t port_ = 0;
  Family family_ = Family::Unspecified;
};

}  // namespace radio::net
