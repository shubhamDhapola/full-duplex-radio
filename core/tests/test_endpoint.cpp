#include "radio/endpoint.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <type_traits>

using namespace radio::net;

TEST_CASE("a default endpoint is unspecified and carries no address") {
  Endpoint e;
  CHECK_FALSE(e.valid());
  CHECK(e.family() == Endpoint::Family::Unspecified);
  CHECK(e.address().empty());
  CHECK(e.to_string() == "<unspecified>");
}

TEST_CASE("IPv4 addresses round-trip through parse and to_string") {
  const auto e = Endpoint::parse("192.168.1.14", 47'000);
  REQUIRE(e.has_value());
  CHECK(e->family() == Endpoint::Family::V4);
  CHECK(e->port() == 47'000);
  CHECK(e->address().size() == 4);
  CHECK(e->to_string() == "192.168.1.14:47000");
}

TEST_CASE("IPv6 addresses round-trip and are bracketed when formatted") {
  const auto e = Endpoint::parse("fe80::1", 47'000);
  REQUIRE(e.has_value());
  CHECK(e->family() == Endpoint::Family::V6);
  CHECK(e->port() == 47'000);
  CHECK(e->address().size() == 16);
  CHECK(e->to_string() == "[fe80::1]:47000");
}

TEST_CASE("host:port strings parse for both families") {
  const auto v4 = Endpoint::parse_with_port("10.0.0.5:1234");
  REQUIRE(v4.has_value());
  CHECK(v4->family() == Endpoint::Family::V4);
  CHECK(v4->port() == 1234);

  // Brackets exist because a bare IPv6 address is full of colons, so
  // "fe80::1:47000" would be genuinely ambiguous.
  const auto v6 = Endpoint::parse_with_port("[2001:db8::42]:9999");
  REQUIRE(v6.has_value());
  CHECK(v6->family() == Endpoint::Family::V6);
  CHECK(v6->port() == 9999);
  CHECK(v6->to_string() == "[2001:db8::42]:9999");
}

TEST_CASE("hostnames are rejected rather than resolved") {
  // Not a missing feature. getaddrinfo() blocks on the network, sometimes for
  // seconds, and a blocking call reachable from the media path is a latency
  // spike waiting to happen. Discovery hands us numeric addresses (M3).
  CHECK_FALSE(Endpoint::parse("myphone.local", 47'000).has_value());
  CHECK_FALSE(Endpoint::parse("localhost", 47'000).has_value());
  CHECK_FALSE(Endpoint::parse_with_port("example.com:80").has_value());
}

TEST_CASE("malformed addresses are rejected") {
  CHECK_FALSE(Endpoint::parse("", 1).has_value());
  CHECK_FALSE(Endpoint::parse("192.168.1", 1).has_value());
  CHECK_FALSE(Endpoint::parse("192.168.1.256", 1).has_value());
  CHECK_FALSE(Endpoint::parse("1.2.3.4.5", 1).has_value());
  CHECK_FALSE(Endpoint::parse("fe80:::1", 1).has_value());
  CHECK_FALSE(Endpoint::parse(std::string(400, 'x'), 1).has_value());
}

TEST_CASE("malformed host:port strings are rejected") {
  CHECK_FALSE(Endpoint::parse_with_port("").has_value());
  CHECK_FALSE(Endpoint::parse_with_port("10.0.0.1").has_value());
  CHECK_FALSE(Endpoint::parse_with_port("10.0.0.1:").has_value());
  CHECK_FALSE(Endpoint::parse_with_port("10.0.0.1:abc").has_value());
  CHECK_FALSE(Endpoint::parse_with_port("10.0.0.1:99999").has_value());
  CHECK_FALSE(Endpoint::parse_with_port("[fe80::1]").has_value());
  CHECK_FALSE(Endpoint::parse_with_port("[fe80::1:47000").has_value());

  // An unbracketed IPv6 address is refused rather than guessed at. Silently
  // picking an interpretation would produce a valid-looking wrong endpoint.
  CHECK_FALSE(Endpoint::parse_with_port("fe80::1:47000").has_value());
}

TEST_CASE("port 0 is valid and means let the kernel choose") {
  const auto e = Endpoint::parse("0.0.0.0", 0);
  REQUIRE(e.has_value());
  CHECK(e->port() == 0);
  CHECK(e->valid());
}

TEST_CASE("equality compares address, port and family") {
  const auto a = Endpoint::parse("10.0.0.1", 5000);
  const auto b = Endpoint::parse("10.0.0.1", 5000);
  const auto different_port = Endpoint::parse("10.0.0.1", 5001);
  const auto different_host = Endpoint::parse("10.0.0.2", 5000);
  REQUIRE(a.has_value());

  CHECK(*a == *b);
  CHECK(*a != *different_port);
  CHECK(*a != *different_host);

  // A V4 address leaves 12 of the 16 address bytes unused. They are
  // zero-initialised, so two equal addresses always compare equal -- which is
  // the reason the type stores plain bytes rather than a sockaddr_storage whose
  // padding is not guaranteed to be zeroed.
  const auto v6 = Endpoint::parse("::ffff:10.0.0.1", 5000);
  REQUIRE(v6.has_value());
  CHECK(*a != *v6);  // same host, different family
}

TEST_CASE("an endpoint is cheap to copy and usable as a value") {
  // A peer table keyed on endpoints is coming in M3, so this matters.
  static_assert(std::is_trivially_copyable_v<Endpoint>);
  const auto a = Endpoint::parse("172.16.0.1", 47'000);
  REQUIRE(a.has_value());
  Endpoint copy = *a;
  CHECK(copy == *a);
}
