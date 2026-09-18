#include "oversub/net.hpp"

#include <cstring>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#if defined(MSG_NOSIGNAL)
#define OVERSUB_SEND_FLAGS MSG_NOSIGNAL
#else
#define OVERSUB_SEND_FLAGS 0
#endif
#endif

namespace oversub {
namespace {

#if defined(_WIN32)
using native_socket = SOCKET;
constexpr native_socket kInvalidSocket = INVALID_SOCKET;

Status last_socket_error(ReasonCode reason, const char* what) {
  return Status::failure(StatusCode::IoError, reason,
                         std::string(what) + " failed with socket error " + std::to_string(::WSAGetLastError()));
}
#else
using native_socket = int;
constexpr native_socket kInvalidSocket = -1;

Status last_socket_error(ReasonCode reason, const char* what) {
  return Status::failure(StatusCode::IoError, reason,
                         std::string(what) + " failed with errno " + std::to_string(errno));
}
#endif

// Process-lifetime network initialization. Winsock requires WSAStartup before any
// socket call, so every entry point initializes it on first use instead of
// requiring callers to hold a NetworkRuntime.
bool ensure_network() {
  static NetworkRuntime runtime;
  return runtime.ok();
}

void close_native(native_socket socket) {
  if (socket == kInvalidSocket) return;
#if defined(_WIN32)
  ::closesocket(socket);
#else
  ::close(socket);
#endif
}

Status fill_address(const std::string& address, u16 port, sockaddr_in& out) {
  std::memset(&out, 0, sizeof(out));
  out.sin_family = AF_INET;
  out.sin_port = htons(port);
  if (address.empty() || address == "localhost") {
    out.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return Status::success();
  }
  if (::inet_pton(AF_INET, address.c_str(), &out.sin_addr) != 1) {
    return Status::failure(StatusCode::InvalidArgument, ReasonCode::InvalidField,
                           "address is not a valid IPv4 literal: " + address);
  }
  return Status::success();
}

void append_u32(std::vector<u8>& out, u32 value) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<u8>((value >> (i * 8)) & 0xFF));
}

bool take_u32(const u8* data, u32& value) {
  value = 0;
  for (int i = 0; i < 4; ++i) value |= static_cast<u32>(data[i]) << (i * 8);
  return true;
}

}  // namespace

NetworkRuntime::NetworkRuntime() {
#if defined(_WIN32)
  WSADATA data;
  ok_ = ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
#else
  ok_ = true;
#endif
}

NetworkRuntime::~NetworkRuntime() {
#if defined(_WIN32)
  if (ok_) ::WSACleanup();
#endif
}

Socket::~Socket() { reset(); }

Socket::Socket(Socket&& other) noexcept : handle_(other.handle_) { other.handle_ = -1; }

Socket& Socket::operator=(Socket&& other) noexcept {
  if (this != &other) {
    reset();
    handle_ = other.handle_;
    other.handle_ = -1;
  }
  return *this;
}

bool Socket::valid() const { return handle_ != -1; }

void Socket::reset() {
  if (handle_ == -1) return;
  close_native(static_cast<native_socket>(handle_));
  handle_ = -1;
}

void Socket::set_nodelay(bool enabled) {
  if (handle_ == -1) return;
  const int value = enabled ? 1 : 0;
  const native_socket socket = static_cast<native_socket>(handle_);
  (void)::setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&value), sizeof(value));
}

Status tcp_listen(const std::string& address, u16 port, Socket& out, u16& bound_port) {
  if (!ensure_network()) {
    return Status::failure(StatusCode::IoError, ReasonCode::TransportError,
                           "network runtime failed to initialize");
  }
  sockaddr_in addr;
  Status status = fill_address(address, port, addr);
  if (!status.ok()) return status;
  const native_socket listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listener == kInvalidSocket) return last_socket_error(ReasonCode::TransportError, "socket");
  {
    const int reuse = 1;
    (void)::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
  }
  if (::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    Status failure = last_socket_error(ReasonCode::TransportError, "bind");
    close_native(listener);
    return failure;
  }
  if (::listen(listener, 16) != 0) {
    Status failure = last_socket_error(ReasonCode::TransportError, "listen");
    close_native(listener);
    return failure;
  }
  sockaddr_in bound;
  std::memset(&bound, 0, sizeof(bound));
#if defined(_WIN32)
  int length = sizeof(bound);
#else
  socklen_t length = sizeof(bound);
#endif
  if (::getsockname(listener, reinterpret_cast<sockaddr*>(&bound), &length) != 0) {
    Status failure = last_socket_error(ReasonCode::TransportError, "getsockname");
    close_native(listener);
    return failure;
  }
  bound_port = ntohs(bound.sin_port);
  out = Socket(static_cast<std::intptr_t>(listener));
  return Status::success();
}

Status tcp_connect(const std::string& address, u16 port, Socket& out) {
  if (!ensure_network()) {
    return Status::failure(StatusCode::IoError, ReasonCode::TransportError,
                           "network runtime failed to initialize");
  }
  sockaddr_in addr;
  Status status = fill_address(address, port, addr);
  if (!status.ok()) return status;
  const native_socket socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (socket == kInvalidSocket) return last_socket_error(ReasonCode::TransportError, "socket");
  if (::connect(socket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    Status failure = last_socket_error(ReasonCode::TransportError, "connect");
    close_native(socket);
    return failure;
  }
  out = Socket(static_cast<std::intptr_t>(socket));
  out.set_nodelay(true);
  return Status::success();
}

Status tcp_accept(const Socket& listener, Socket& out, std::string& peer) {
  if (!listener.valid()) {
    return Status::failure(StatusCode::InvalidArgument, ReasonCode::TransportClosed, "listener is not open");
  }
  sockaddr_in remote;
  std::memset(&remote, 0, sizeof(remote));
#if defined(_WIN32)
  int length = sizeof(remote);
#else
  socklen_t length = sizeof(remote);
#endif
  const native_socket accepted = ::accept(static_cast<native_socket>(listener.native()),
                                          reinterpret_cast<sockaddr*>(&remote), &length);
  if (accepted == kInvalidSocket) {
    return last_socket_error(ReasonCode::TransportClosed, "accept");
  }
  char text[INET_ADDRSTRLEN] = {0};
  if (::inet_ntop(AF_INET, &remote.sin_addr, text, sizeof(text)) == nullptr) {
    // text is zero-initialized and the literal fits, so the result is always
    // terminated; the analyzer-flagged strncpy form is not used.
    static const char kUnknown[] = "unknown";
    static_assert(sizeof(kUnknown) <= sizeof(text), "peer label must fit the buffer");
    std::memcpy(text, kUnknown, sizeof(kUnknown));
  }
  peer = std::string(text) + ":" + std::to_string(ntohs(remote.sin_port));
  out = Socket(static_cast<std::intptr_t>(accepted));
  out.set_nodelay(true);
  return Status::success();
}

FramedConnection::FramedConnection(Socket socket) : socket_(std::move(socket)) {
  scratch_.reserve(4096);
}

void FramedConnection::close() {
  closed_ = true;
  socket_.reset();
}

Status FramedConnection::read_exact(void* out, std::size_t length) {
  u8* cursor = static_cast<u8*>(out);
  std::size_t remaining = length;
  while (remaining > 0) {
    const native_socket socket = static_cast<native_socket>(socket_.native());
    if (socket == kInvalidSocket) {
      return Status::failure(StatusCode::IoError, ReasonCode::TransportClosed, "socket is closed");
    }
#if defined(_WIN32)
    const int chunk = ::recv(socket, reinterpret_cast<char*>(cursor),
                             static_cast<int>(remaining > 0x7FFFFFFF ? 0x7FFFFFFF : remaining), 0);
#else
    const ssize_t chunk = ::recv(socket, cursor, remaining, 0);
#endif
    if (chunk == 0) {
      if (remaining == length) {
        return Status::failure(StatusCode::IoError, ReasonCode::TransportClosed, "peer closed the connection");
      }
      return Status::failure(StatusCode::IoError, ReasonCode::FrameTruncated,
                             "peer closed the connection mid-frame");
    }
    if (chunk < 0) {
      return last_socket_error(ReasonCode::TransportError, "recv");
    }
    cursor += chunk;
    remaining -= static_cast<std::size_t>(chunk);
  }
  return Status::success();
}

Status FramedConnection::send_frame(u32 type, const std::vector<u8>& payload) {
  if (closed_) {
    return Status::failure(StatusCode::IoError, ReasonCode::TransportClosed, "connection is closed");
  }
  if (payload.size() > kMaxFramePayloadBytes) {
    return Status::failure(StatusCode::OutOfRange, ReasonCode::FrameTooLarge, "frame payload exceeds the bound");
  }
  std::vector<u8> header;
  header.reserve(8);
  append_u32(header, type);
  append_u32(header, static_cast<u32>(payload.size()));
  const native_socket socket = static_cast<native_socket>(socket_.native());
  if (socket == kInvalidSocket) {
    return Status::failure(StatusCode::IoError, ReasonCode::TransportClosed, "socket is closed");
  }
  auto send_all = [&](const u8* data, std::size_t length) -> Status {
    std::size_t offset = 0;
    while (offset < length) {
      const std::size_t remaining = length - offset;
#if defined(_WIN32)
      const int chunk = ::send(socket, reinterpret_cast<const char*>(data + offset),
                               static_cast<int>(remaining > 0x7FFFFFFF ? 0x7FFFFFFF : remaining), 0);
#else
      const ssize_t chunk = ::send(socket, data + offset, remaining, OVERSUB_SEND_FLAGS);
#endif
      if (chunk <= 0) return last_socket_error(ReasonCode::TransportError, "send");
      offset += static_cast<std::size_t>(chunk);
    }
    return Status::success();
  };
  Status status = send_all(header.data(), header.size());
  if (!status.ok()) return status;
  if (!payload.empty()) status = send_all(payload.data(), payload.size());
  return status;
}

Status FramedConnection::recv_frame(u32& type, std::vector<u8>& payload) {
  payload.clear();
  if (closed_) {
    return Status::failure(StatusCode::IoError, ReasonCode::TransportClosed, "connection is closed");
  }
  u8 header[8];
  Status status = read_exact(header, sizeof(header));
  if (!status.ok()) {
    if (status.reason == ReasonCode::TransportClosed) closed_ = true;
    return status;
  }
  u32 frame_type = 0;
  u32 length = 0;
  take_u32(header, frame_type);
  take_u32(header + 4, length);
  if (length > kMaxFramePayloadBytes) {
    return Status::failure(StatusCode::OutOfRange, ReasonCode::FrameTooLarge,
                           "declared frame payload exceeds the bound");
  }
  type = frame_type;
  payload.resize(length);
  if (length == 0) return Status::success();
  status = read_exact(payload.data(), payload.size());
  if (!status.ok()) return status;
  return Status::success();
}

}  // namespace oversub
