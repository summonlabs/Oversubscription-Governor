// Framed transport tests over real loopback TCP sockets.
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <sys/socket.h>
#endif

#include "framework.hpp"
#include "oversub/net.hpp"
#include "testutil.hpp"

using namespace oversub;
using namespace otest;

namespace {

// Raw byte writes, used to exercise fragmented and malformed framing that the
// FramedConnection API deliberately never produces.
bool raw_send(const FramedConnection& connection, const std::vector<u8>& bytes) {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const std::size_t remaining = bytes.size() - offset;
#if defined(_WIN32)
    const int chunk = ::send(static_cast<SOCKET>(connection.native()),
                             reinterpret_cast<const char*>(bytes.data() + offset),
                             static_cast<int>(remaining), 0);
#else
    const ssize_t chunk = ::send(connection.native(), bytes.data() + offset, remaining, 0);
#endif
    if (chunk <= 0) return false;
    offset += static_cast<std::size_t>(chunk);
  }
  return true;
}

std::vector<u8> header_bytes(u32 type, u32 length) {
  std::vector<u8> out;
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<u8>((type >> (i * 8)) & 0xFF));
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<u8>((length >> (i * 8)) & 0xFF));
  return out;
}

struct Pair {
  Socket listener;
  FramedConnection server;
  FramedConnection client;
  u16 port{0};
};

bool make_pair(Pair& pair) {
  if (!tcp_listen("127.0.0.1", 0, pair.listener, pair.port).ok()) return false;
  Socket accepted;
  std::string peer;
  std::thread acceptor([&] { (void)tcp_accept(pair.listener, accepted, peer); });
  Socket client;
  const Status connected = tcp_connect("127.0.0.1", pair.port, client);
  acceptor.join();
  if (!connected.ok() || !accepted.valid()) return false;
  pair.server = FramedConnection(std::move(accepted));
  pair.client = FramedConnection(std::move(client));
  return true;
}

}  // namespace

TEST(net_frames_round_trip_with_bounded_payloads) {
  NetworkRuntime runtime;
  REQUIRE(runtime.ok());
  Pair pair;
  REQUIRE(make_pair(pair));
  const std::vector<std::size_t> sizes = {0, 1, 7, 4096, 65536, kMaxFramePayloadBytes};
  for (const std::size_t size : sizes) {
    std::vector<u8> payload(size);
    for (std::size_t i = 0; i < size; ++i) payload[i] = static_cast<u8>((i * 31) & 0xFF);
    REQUIRE(pair.client.send_frame(7, payload).ok());
    u32 type = 0;
    std::vector<u8> received;
    REQUIRE(pair.server.recv_frame(type, received).ok());
    CHECK_EQ(type, u32{7});
    CHECK(received == payload);
  }
  pair.client.close();
  pair.server.close();
}

TEST(net_rejects_oversized_frames_in_both_directions) {
  NetworkRuntime runtime;
  REQUIRE(runtime.ok());
  Pair pair;
  REQUIRE(make_pair(pair));
  std::vector<u8> too_big(kMaxFramePayloadBytes + 1);
  const Status send_status = pair.client.send_frame(1, too_big);
  CHECK(!send_status.ok());
  CHECK_EQ(send_status.reason, ReasonCode::FrameTooLarge);
  // A hostile peer that declares an oversized frame is rejected before reading.
  REQUIRE(raw_send(pair.client, header_bytes(1, kMaxFramePayloadBytes + 1)));
  u32 type = 0;
  std::vector<u8> payload;
  const Status recv_status = pair.server.recv_frame(type, payload);
  CHECK(!recv_status.ok());
  CHECK_EQ(recv_status.reason, ReasonCode::FrameTooLarge);
  pair.client.close();
  pair.server.close();
}

TEST(net_detects_clean_close_and_mid_frame_truncation) {
  NetworkRuntime runtime;
  REQUIRE(runtime.ok());
  {
    Pair pair;
    REQUIRE(make_pair(pair));
    pair.client.close();
    u32 type = 0;
    std::vector<u8> payload;
    const Status status = pair.server.recv_frame(type, payload);
    CHECK(!status.ok());
    CHECK_EQ(status.reason, ReasonCode::TransportClosed);
    CHECK(pair.server.closed());
  }
  {
    Pair pair;
    REQUIRE(make_pair(pair));
    // Declare 100 bytes, send 10, then close.
    REQUIRE(raw_send(pair.client, header_bytes(3, 100)));
    std::vector<u8> partial(10, 0x11);
    REQUIRE(raw_send(pair.client, partial));
    pair.client.close();
    u32 type = 0;
    std::vector<u8> payload;
    const Status status = pair.server.recv_frame(type, payload);
    CHECK(!status.ok());
    CHECK_EQ(status.reason, ReasonCode::FrameTruncated);
  }
}

TEST(net_reassembles_fragmented_frames) {
  NetworkRuntime runtime;
  REQUIRE(runtime.ok());
  Pair pair;
  REQUIRE(make_pair(pair));
  std::vector<u8> payload(5000);
  for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<u8>(i % 251);
  std::vector<u8> frame = header_bytes(9, static_cast<u32>(payload.size()));
  frame.insert(frame.end(), payload.begin(), payload.end());
  std::thread writer([&] {
    std::vector<u8> first(frame.begin(), frame.begin() + 2000);
    (void)raw_send(pair.client, first);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    std::vector<u8> second(frame.begin() + 2000, frame.end());
    (void)raw_send(pair.client, second);
  });
  u32 type = 0;
  std::vector<u8> received;
  const Status status = pair.server.recv_frame(type, received);
  writer.join();
  REQUIRE(status.ok());
  CHECK_EQ(type, u32{9});
  CHECK(received == payload);
  pair.client.close();
  pair.server.close();
}

TEST(net_serves_multiple_sequential_connections) {
  NetworkRuntime runtime;
  REQUIRE(runtime.ok());
  Socket listener;
  u16 port = 0;
  REQUIRE(tcp_listen("127.0.0.1", 0, listener, port).ok());
  constexpr int kClients = 8;
  std::thread server([&] {
    for (int i = 0; i < kClients; ++i) {
      Socket accepted;
      std::string peer;
      if (!tcp_accept(listener, accepted, peer).ok()) return;
      FramedConnection connection(std::move(accepted));
      connection.set_peer(peer);
      u32 type = 0;
      std::vector<u8> payload;
      if (!connection.recv_frame(type, payload).ok()) return;
      std::vector<u8> response(payload.rbegin(), payload.rend());
      (void)connection.send_frame(type + 1, response);
      connection.close();
    }
  });
  for (int i = 0; i < kClients; ++i) {
    Socket socket;
    REQUIRE(tcp_connect("127.0.0.1", port, socket).ok());
    FramedConnection connection(std::move(socket));
    const std::vector<u8> payload{1, 2, 3, static_cast<u8>(i)};
    REQUIRE(connection.send_frame(11, payload).ok());
    u32 type = 0;
    std::vector<u8> response;
    REQUIRE(connection.recv_frame(type, response).ok());
    CHECK_EQ(type, u32{12});
    CHECK_EQ(response.size(), payload.size());
    CHECK_EQ(response[0], payload[payload.size() - 1]);
    connection.close();
  }
  server.join();
  listener.reset();
}

TEST(net_concurrent_clients_do_not_interleave_frames) {
  NetworkRuntime runtime;
  REQUIRE(runtime.ok());
  Socket listener;
  u16 port = 0;
  REQUIRE(tcp_listen("127.0.0.1", 0, listener, port).ok());
  constexpr int kClients = 4;
  constexpr int kFrames = 40;
  std::thread server([&] {
    for (int i = 0; i < kClients; ++i) {
      Socket accepted;
      std::string peer;
      if (!tcp_accept(listener, accepted, peer).ok()) return;
      std::thread handler([socket = std::move(accepted)]() mutable {
        FramedConnection connection(std::move(socket));
        for (int f = 0; f < kFrames; ++f) {
          u32 type = 0;
          std::vector<u8> payload;
          if (!connection.recv_frame(type, payload).ok()) return;
          if (!connection.send_frame(type, payload).ok()) return;
        }
        connection.close();
      });
      handler.detach();
    }
  });
  std::vector<std::thread> clients;
  for (int c = 0; c < kClients; ++c) {
    clients.emplace_back([&, c] {
      Socket socket;
      if (!tcp_connect("127.0.0.1", port, socket).ok()) return;
      FramedConnection connection(std::move(socket));
      for (int f = 0; f < kFrames; ++f) {
        std::vector<u8> payload(64, static_cast<u8>(c));
        payload[0] = static_cast<u8>(f);
        if (!connection.send_frame(21, payload).ok()) return;
        u32 type = 0;
        std::vector<u8> response;
        if (!connection.recv_frame(type, response).ok()) return;
        if (!(response == payload)) ::otest::fail("frame payload was corrupted in flight");
      }
      connection.close();
    });
  }
  for (auto& client : clients) client.join();
  server.join();
  listener.reset();
}

TEST(net_connect_to_a_closed_port_fails) {
  NetworkRuntime runtime;
  REQUIRE(runtime.ok());
  Socket listener;
  u16 port = 0;
  REQUIRE(tcp_listen("127.0.0.1", 0, listener, port).ok());
  listener.reset();
  Socket socket;
  const Status status = tcp_connect("127.0.0.1", port, socket);
  CHECK(!status.ok());
  CHECK(!socket.valid());
}

TEST(net_rejects_invalid_addresses) {
  NetworkRuntime runtime;
  REQUIRE(runtime.ok());
  Socket listener;
  u16 port = 0;
  const Status status = tcp_listen("not-an-address", 0, listener, port);
  CHECK(!status.ok());
  CHECK_EQ(status.code, StatusCode::InvalidArgument);
}
