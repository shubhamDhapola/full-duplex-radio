#include "radio/udp_socket.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cerrno>
#include <cstring>
#include <string_view>
#include <type_traits>
#include <utility>

using namespace radio;
using namespace radio::net;

namespace {

ByteView bytes_of(std::string_view text) {
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

std::string_view text_of(ByteView view) {
  return {reinterpret_cast<const char*>(view.data()), view.size()};
}

// Two sockets bound to ephemeral loopback ports, ready to talk to each other.
struct Pair {
  UdpSocket a;
  UdpSocket b;
  Endpoint a_address;
  Endpoint b_address;

  static Pair make(Endpoint::Family family = Endpoint::Family::V4) {
    Pair p;
    UdpSocket::Options options;
    options.family = family;
    REQUIRE(p.a.open(options));
    REQUIRE(p.b.open(options));

    const char* loopback = family == Endpoint::Family::V6 ? "::1" : "127.0.0.1";
    p.a_address = *Endpoint::parse(loopback, p.a.local_endpoint().port());
    p.b_address = *Endpoint::parse(loopback, p.b.local_endpoint().port());
    return p;
  }
};

}  // namespace

TEST_CASE("a socket handle is movable but not copyable") {
  // Copying would give two objects the same descriptor, and both destructors
  // would close it. The second close would target a number the kernel has since
  // reissued to another file, sending writes somewhere entirely unrelated.
  static_assert(!std::is_copy_constructible_v<UdpSocket>);
  static_assert(!std::is_copy_assignable_v<UdpSocket>);
  static_assert(std::is_move_constructible_v<UdpSocket>);
  static_assert(std::is_move_assignable_v<UdpSocket>);
  SUCCEED();
}

TEST_CASE("a default-constructed socket owns nothing") {
  UdpSocket s;
  CHECK_FALSE(s.is_open());

  std::array<std::byte, 64> buffer{};
  const auto received = s.recv_from(buffer);
  CHECK(received.error == EBADF);

  const auto sent = s.send_to(*Endpoint::parse("127.0.0.1", 9), bytes_of("x"));
  CHECK(sent.error == EBADF);
}

TEST_CASE("binding to port 0 yields an ephemeral port we can read back") {
  UdpSocket s;
  REQUIRE(s.open(UdpSocket::Options{}));
  CHECK(s.is_open());
  CHECK(s.local_endpoint().valid());
  // getsockname is the only way to learn what the kernel picked.
  CHECK(s.local_endpoint().port() != 0);
}

TEST_CASE("a datagram survives a loopback round trip intact") {
  auto pair = Pair::make();
  const std::string_view message = "protocol v1 media frame";

  const auto sent = pair.a.send_to(pair.b_address, bytes_of(message));
  REQUIRE(sent.ok());
  CHECK(sent.bytes == message.size());

  REQUIRE(pair.b.wait_readable(1'000));

  std::array<std::byte, 256> buffer{};
  const auto received = pair.b.recv_from(buffer);
  REQUIRE(received.ok());
  CHECK(received.bytes == message.size());
  CHECK(text_of(ByteView{buffer.data(), received.bytes}) == message);

  // The source address identifies the peer, which is half of the
  // (source, stream_id) key the protocol uses for stream state.
  CHECK(received.from.port() == pair.a.local_endpoint().port());
  CHECK(received.from.family() == Endpoint::Family::V4);
}

TEST_CASE(
    "receiving from an empty socket returns EAGAIN rather than blocking") {
  // The proof that the socket is non-blocking. A blocking recvfrom would park
  // the thread here and this test would hang instead of failing.
  UdpSocket s;
  REQUIRE(s.open(UdpSocket::Options{}));

  std::array<std::byte, 64> buffer{};
  const auto received = s.recv_from(buffer);
  CHECK_FALSE(received.ok());
  CHECK(received.would_block());
  // A would-block is a normal empty poll, not a fault, so it must not be
  // counted as an error.
  CHECK(s.stats().recv_errors == 0);
}

TEST_CASE("the arrival timestamp is taken during the receive call") {
  auto pair = Pair::make();

  const Micros before = now_us();
  REQUIRE(pair.a.send_to(pair.b_address, bytes_of("timed")).ok());
  REQUIRE(pair.b.wait_readable(1'000));

  std::array<std::byte, 64> buffer{};
  const auto received = pair.b.recv_from(buffer);
  const Micros after = now_us();

  REQUIRE(received.ok());
  CHECK(received.arrival_us >= before);
  CHECK(received.arrival_us <= after);

  // Loopback, so the whole exchange is well under a millisecond. If this ever
  // grows, something is doing work between the syscall and the clock read --
  // and that work would be reported as network jitter.
  CHECK(after - before < 100'000);
}

TEST_CASE("wait_readable honours its timeout when nothing arrives") {
  UdpSocket s;
  REQUIRE(s.open(UdpSocket::Options{}));

  const Micros start = now_us();
  CHECK_FALSE(s.wait_readable(20));
  const Micros elapsed = now_us() - start;

  CHECK(elapsed >= 15'000);  // waited roughly the requested time
  CHECK(elapsed < 500'000);  // and returned rather than hanging
}

TEST_CASE("an oversized datagram is truncated and the truncation is reported") {
  // recvfrom would discard the tail silently; the resulting stream of
  // malformed-packet counters would give no hint of the real cause. recvmsg
  // exposes MSG_TRUNC, so the condition is visible where it happens.
  auto pair = Pair::make();
  const std::array<std::byte, 512> large{};

  REQUIRE(pair.a.send_to(pair.b_address, large).ok());
  REQUIRE(pair.b.wait_readable(1'000));

  std::array<std::byte, 16> small{};
  const auto received = pair.b.recv_from(small);
  REQUIRE(received.ok());
  CHECK(received.bytes == small.size());
  CHECK(pair.b.stats().truncated == 1);
}

TEST_CASE("moving a socket transfers the descriptor and disarms the source") {
  auto pair = Pair::make();
  const auto original_port = pair.a.local_endpoint().port();

  UdpSocket moved = std::move(pair.a);
  CHECK_FALSE(pair.a.is_open());
  CHECK(moved.is_open());
  CHECK(moved.local_endpoint().port() == original_port);

  // The descriptor still works after the move, which is what proves the
  // moved-from object's destructor did not close it.
  REQUIRE(moved.send_to(pair.b_address, bytes_of("after move")).ok());
  REQUIRE(pair.b.wait_readable(1'000));
  std::array<std::byte, 64> buffer{};
  const auto received = pair.b.recv_from(buffer);
  REQUIRE(received.ok());
  CHECK(text_of(ByteView{buffer.data(), received.bytes}) == "after move");
  CHECK(received.from.port() == original_port);
}

TEST_CASE("move assignment closes whatever the target already held") {
  UdpSocket first;
  UdpSocket second;
  REQUIRE(first.open(UdpSocket::Options{}));
  REQUIRE(second.open(UdpSocket::Options{}));
  const auto second_port = second.local_endpoint().port();

  first = std::move(second);
  CHECK(first.is_open());
  CHECK(first.local_endpoint().port() == second_port);
  CHECK_FALSE(second.is_open());
}

TEST_CASE("self-move-assignment does not destroy the socket") {
  // Pathological but reachable through a generic algorithm, and getting it
  // wrong closes the descriptor and then keeps using it.
  UdpSocket s;
  REQUIRE(s.open(UdpSocket::Options{}));
  const auto port = s.local_endpoint().port();

  UdpSocket& alias = s;
  s = std::move(alias);

  CHECK(s.is_open());
  CHECK(s.local_endpoint().port() == port);
}

TEST_CASE("closing is idempotent and leaves the object usable again") {
  UdpSocket s;
  REQUIRE(s.open(UdpSocket::Options{}));
  s.close();
  CHECK_FALSE(s.is_open());
  s.close();
  CHECK_FALSE(s.is_open());

  REQUIRE(s.open(UdpSocket::Options{}));
  CHECK(s.is_open());
}

TEST_CASE(
    "sending to an unspecified endpoint fails without touching the wire") {
  UdpSocket s;
  REQUIRE(s.open(UdpSocket::Options{}));

  const auto sent = s.send_to(Endpoint{}, bytes_of("nowhere"));
  CHECK(sent.error == EINVAL);
  CHECK(s.stats().send_errors == 1);
  CHECK(s.stats().datagrams_sent == 0);
}

TEST_CASE("statistics count datagrams and bytes in both directions") {
  auto pair = Pair::make();
  std::array<std::byte, 128> buffer{};

  for (int i = 0; i < 5; ++i) {
    REQUIRE(pair.a.send_to(pair.b_address, bytes_of("12345678")).ok());
  }
  for (int i = 0; i < 5; ++i) {
    REQUIRE(pair.b.wait_readable(1'000));
    REQUIRE(pair.b.recv_from(buffer).ok());
  }

  CHECK(pair.a.stats().datagrams_sent == 5);
  CHECK(pair.a.stats().bytes_sent == 40);
  CHECK(pair.b.stats().datagrams_received == 5);
  CHECK(pair.b.stats().bytes_received == 40);
  CHECK(pair.b.stats().truncated == 0);

  pair.b.reset_stats();
  CHECK(pair.b.stats().datagrams_received == 0);
}

TEST_CASE("IPv6 loopback works when the host supports it") {
  UdpSocket probe;
  UdpSocket::Options options;
  options.family = Endpoint::Family::V6;
  if (!probe.open(options)) {
    SUCCEED("IPv6 unavailable on this host");
    return;
  }
  probe.close();

  auto pair = Pair::make(Endpoint::Family::V6);
  REQUIRE(pair.a.send_to(pair.b_address, bytes_of("v6 frame")).ok());
  REQUIRE(pair.b.wait_readable(1'000));

  std::array<std::byte, 64> buffer{};
  const auto received = pair.b.recv_from(buffer);
  REQUIRE(received.ok());
  CHECK(text_of(ByteView{buffer.data(), received.bytes}) == "v6 frame");
  CHECK(received.from.family() == Endpoint::Family::V6);
}

TEST_CASE("a specific bind address is honoured") {
  UdpSocket s;
  UdpSocket::Options options;
  options.bind_address = *Endpoint::parse("127.0.0.1", 0);
  REQUIRE(s.open(options));
  CHECK(s.local_endpoint().to_string().starts_with("127.0.0.1:"));
}
