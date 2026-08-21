#include "radio/endpoint.hpp"

#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <charconv>
#include <cstring>

namespace radio::net {
namespace {

constexpr std::size_t kV4Bytes = 4;
constexpr std::size_t kV6Bytes = 16;

}  // namespace

Endpoint Endpoint::v4(std::array<std::byte, 4> address,
                      std::uint16_t port) noexcept {
  Endpoint e;
  std::memcpy(e.address_.data(), address.data(), kV4Bytes);
  e.port_ = port;
  e.family_ = Family::V4;
  return e;
}

Endpoint Endpoint::v6(std::array<std::byte, 16> address, std::uint16_t port,
                      std::uint32_t scope_id) noexcept {
  Endpoint e;
  e.address_ = address;
  e.port_ = port;
  e.scope_id_ = scope_id;
  e.family_ = Family::V6;
  return e;
}

std::optional<Endpoint> Endpoint::parse(std::string_view address,
                                        std::uint16_t port) noexcept {
  // inet_pton needs a NUL-terminated string and string_view has no guarantee of
  // one, so the text is copied into a bounded stack buffer. No allocation, and
  // an over-long input is rejected rather than truncated into something that
  // might parse as a different address.
  char buffer[INET6_ADDRSTRLEN + 32] = {};
  if (address.empty() || address.size() >= sizeof(buffer)) return std::nullopt;
  std::memcpy(buffer, address.data(), address.size());

  // A scope suffix ("fe80::1%en0") is meaningful for link-local IPv6, which is
  // exactly what a LAN uses, so it is parsed rather than rejected.
  std::uint32_t scope_id = 0;
  if (char* percent = std::strchr(buffer, '%'); percent != nullptr) {
    *percent = '\0';
    scope_id = ::if_nametoindex(percent + 1);
    if (scope_id == 0) return std::nullopt;
  }

  in_addr v4{};
  if (::inet_pton(AF_INET, buffer, &v4) == 1) {
    std::array<std::byte, kV4Bytes> bytes{};
    std::memcpy(bytes.data(), &v4.s_addr, kV4Bytes);
    return Endpoint::v4(bytes, port);
  }

  in6_addr v6{};
  if (::inet_pton(AF_INET6, buffer, &v6) == 1) {
    std::array<std::byte, kV6Bytes> bytes{};
    std::memcpy(bytes.data(), &v6.s6_addr, kV6Bytes);
    return Endpoint::v6(bytes, port, scope_id);
  }

  return std::nullopt;
}

std::optional<Endpoint> Endpoint::parse_with_port(
    std::string_view text) noexcept {
  if (text.empty()) return std::nullopt;

  std::string_view host;
  std::string_view port_text;

  if (text.front() == '[') {
    // Bracketed IPv6: the brackets exist precisely because a bare IPv6 address
    // is full of colons and "fe80::1:47000" is ambiguous.
    const auto close = text.find(']');
    if (close == std::string_view::npos) return std::nullopt;
    host = text.substr(1, close - 1);
    const auto rest = text.substr(close + 1);
    if (rest.size() < 2 || rest.front() != ':') return std::nullopt;
    port_text = rest.substr(1);
  } else {
    const auto colon = text.rfind(':');
    if (colon == std::string_view::npos) return std::nullopt;
    host = text.substr(0, colon);
    port_text = text.substr(colon + 1);
    // An unbracketed string with several colons is an IPv6 address missing its
    // brackets. Guessing would silently produce the wrong endpoint.
    if (host.find(':') != std::string_view::npos) return std::nullopt;
  }

  std::uint16_t port = 0;
  const auto* begin = port_text.data();
  const auto* end = begin + port_text.size();
  const auto result = std::from_chars(begin, end, port);
  if (result.ec != std::errc{} || result.ptr != end) return std::nullopt;

  return parse(host, port);
}

std::span<const std::byte> Endpoint::address() const noexcept {
  switch (family_) {
    case Family::V4:
      return {address_.data(), kV4Bytes};
    case Family::V6:
      return {address_.data(), kV6Bytes};
    case Family::Unspecified:
      break;
  }
  return {};
}

std::string Endpoint::to_string() const {
  char text[INET6_ADDRSTRLEN] = {};

  switch (family_) {
    case Family::V4: {
      in_addr v4{};
      std::memcpy(&v4.s_addr, address_.data(), kV4Bytes);
      if (::inet_ntop(AF_INET, &v4, text, sizeof(text)) == nullptr) {
        return "<invalid>";
      }
      return std::string(text) + ":" + std::to_string(port_);
    }
    case Family::V6: {
      in6_addr v6{};
      std::memcpy(&v6.s6_addr, address_.data(), kV6Bytes);
      if (::inet_ntop(AF_INET6, &v6, text, sizeof(text)) == nullptr) {
        return "<invalid>";
      }
      std::string out = "[";
      out += text;
      if (scope_id_ != 0) {
        char name[64] = {};
        if (::if_indextoname(scope_id_, name) != nullptr) {
          out += "%";
          out += name;
        }
      }
      out += "]:";
      out += std::to_string(port_);
      return out;
    }
    case Family::Unspecified:
      break;
  }
  return "<unspecified>";
}

}  // namespace radio::net
