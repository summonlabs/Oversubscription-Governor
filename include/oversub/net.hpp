// Oversubscription Governor — framed TCP transport for evidence publishers.
//
// Frames are length-prefixed and hard-bounded. The transport carries governance
// messages only; it never carries authoritative state implicitly.
#ifndef OVERSUB_NET_HPP
#define OVERSUB_NET_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "oversub/checked.hpp"
#include "oversub/status.hpp"

namespace oversub {

inline constexpr u32 kMaxFramePayloadBytes = 1u << 20;   // 1 MiB
inline constexpr u32 kProtocolVersion = 1;

// Process-lifetime network runtime (Winsock on Windows, no-op elsewhere).
// Sockets initialize it on first use, so holding one is optional.
class NetworkRuntime {
 public:
  NetworkRuntime();
  ~NetworkRuntime();
  NetworkRuntime(const NetworkRuntime&) = delete;
  NetworkRuntime& operator=(const NetworkRuntime&) = delete;
  bool ok() const { return ok_; }

 private:
  bool ok_{false};
};

class Socket {
 public:
  Socket() = default;
  explicit Socket(std::intptr_t handle) : handle_(handle) {}
  ~Socket();
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
  Socket(Socket&& other) noexcept;
  Socket& operator=(Socket&& other) noexcept;

  bool valid() const;
  std::intptr_t native() const { return handle_; }
  void reset();
  void set_nodelay(bool enabled);

 private:
  std::intptr_t handle_{-1};
};

// Binds a listening socket. Port 0 selects an ephemeral port, reported back.
Status tcp_listen(const std::string& address, u16 port, Socket& out, u16& bound_port);
Status tcp_connect(const std::string& address, u16 port, Socket& out);
// Blocks until a connection arrives or the listener is closed.
Status tcp_accept(const Socket& listener, Socket& out, std::string& peer);

class FramedConnection {
 public:
  FramedConnection() = default;
  explicit FramedConnection(Socket socket);
  FramedConnection(const FramedConnection&) = delete;
  FramedConnection& operator=(const FramedConnection&) = delete;
  FramedConnection(FramedConnection&&) noexcept = default;
  FramedConnection& operator=(FramedConnection&&) noexcept = default;

  // Sends one frame. Messages longer than kMaxFramePayloadBytes are rejected.
  Status send_frame(u32 type, const std::vector<u8>& payload);
  // Receives one frame. Returns TransportClosed on a clean end of stream and
  // FrameTruncated when the peer disappears mid-frame.
  Status recv_frame(u32& type, std::vector<u8>& payload);
  void close();
  bool closed() const { return closed_; }
  std::intptr_t native() const { return socket_.native(); }
  const std::string& peer() const { return peer_; }
  void set_peer(std::string peer) { peer_ = std::move(peer); }

 private:
  Status read_exact(void* out, std::size_t length);
  Socket socket_;
  std::string peer_;
  std::vector<u8> scratch_;
  bool closed_{false};
};

}  // namespace oversub

#endif  // OVERSUB_NET_HPP
