#include "net/socket.h"

#include <cstring>
#include <mutex>

#include "base/log.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using native_socket = SOCKET;
#define RC_CLOSE_SOCKET closesocket
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>
using native_socket = int;
#define RC_CLOSE_SOCKET ::close
#endif

namespace rc {
namespace {

native_socket to_native(intptr_t fd) { return static_cast<native_socket>(fd); }

intptr_t from_native(native_socket fd) { return static_cast<intptr_t>(fd); }

#ifdef _WIN32
std::once_flag g_wsa_once;
#endif

}  // namespace

void net_init() {
#ifdef _WIN32
  std::call_once(g_wsa_once, [] {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
      RC_LOGE("net", "WSAStartup failed");
    }
  });
#endif
}

void TcpSocket::close() {
  if (fd_ == -1) return;
  RC_CLOSE_SOCKET(to_native(fd_));
  fd_ = -1;
}

bool TcpSocket::send_all(const void* data, std::size_t length) {
  const char* cursor = static_cast<const char*>(data);
  std::size_t remaining = length;
  while (remaining > 0) {
    const int sent = static_cast<int>(
        ::send(to_native(fd_), cursor, static_cast<int>(remaining), 0));
    if (sent <= 0) return false;
    cursor += sent;
    remaining -= static_cast<std::size_t>(sent);
  }
  return true;
}

bool TcpSocket::send_vectored(const uint8_t* const* buffers, const std::size_t* sizes,
                              int count) {
  constexpr int kMaxVectors = 3;
  if (count <= 0 || count > kMaxVectors) return false;

  int first = 0;        // 当前待发段
  std::size_t skip = 0;  // 当前段内已发出的字节数

  while (first < count) {
#ifdef _WIN32
    WSABUF bufs[kMaxVectors];
    int nbufs = 0;
    for (int i = first; i < count; ++i) {
      const std::size_t begin = (i == first) ? skip : 0;
      if (sizes[i] <= begin) continue;
      bufs[nbufs].buf = reinterpret_cast<char*>(const_cast<uint8_t*>(buffers[i])) + begin;
      bufs[nbufs].len = static_cast<ULONG>(sizes[i] - begin);
      ++nbufs;
    }
    if (nbufs == 0) return true;
    DWORD sent = 0;
    if (::WSASend(to_native(fd_), bufs, static_cast<DWORD>(nbufs), &sent, 0, nullptr,
                  nullptr) != 0) {
      return false;
    }
    std::size_t written = static_cast<std::size_t>(sent);
#else
    struct iovec iov[kMaxVectors];
    int nbufs = 0;
    for (int i = first; i < count; ++i) {
      const std::size_t begin = (i == first) ? skip : 0;
      if (sizes[i] <= begin) continue;
      iov[nbufs].iov_base = const_cast<uint8_t*>(buffers[i]) + begin;
      iov[nbufs].iov_len = sizes[i] - begin;
      ++nbufs;
    }
    if (nbufs == 0) return true;
    struct msghdr msg {};
    msg.msg_iov = iov;
    msg.msg_iovlen = nbufs;
    const ssize_t sent = ::sendmsg(to_native(fd_), &msg, 0);
    if (sent <= 0) return false;
    std::size_t written = static_cast<std::size_t>(sent);
#endif
    if (written == 0) return false;

    // 按实际写入量推进段游标（处理部分写，通常是发送缓冲满）。
    while (written > 0 && first < count) {
      const std::size_t available = sizes[first] - skip;
      if (written >= available) {
        written -= available;
        ++first;
        skip = 0;
      } else {
        skip += written;
        written = 0;
      }
    }
  }
  return true;
}

bool TcpSocket::recv_exact(void* data, std::size_t length) {
  char* cursor = static_cast<char*>(data);
  std::size_t remaining = length;
  while (remaining > 0) {
    const int got = static_cast<int>(
        ::recv(to_native(fd_), cursor, static_cast<int>(remaining), 0));
    if (got <= 0) return false;
    cursor += got;
    remaining -= static_cast<std::size_t>(got);
  }
  return true;
}

void TcpSocket::set_recv_timeout_ms(int milliseconds) {
#ifdef _WIN32
  DWORD timeout = static_cast<DWORD>(milliseconds);
#else
  timeval timeout{};
  timeout.tv_sec = milliseconds / 1000;
  timeout.tv_usec = (milliseconds % 1000) * 1000;
#endif
  ::setsockopt(to_native(fd_), SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));
}

void TcpSocket::set_send_timeout_ms(int milliseconds) {
#ifdef _WIN32
  DWORD timeout = static_cast<DWORD>(milliseconds);
#else
  timeval timeout{};
  timeout.tv_sec = milliseconds / 1000;
  timeout.tv_usec = (milliseconds % 1000) * 1000;
#endif
  ::setsockopt(to_native(fd_), SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));
}

TcpSocket TcpSocket::connect(const std::string& host, uint16_t port,
                             std::string* error) {
  net_init();

  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  const std::string port_text = std::to_string(port);
  addrinfo* results = nullptr;
  const int rc = ::getaddrinfo(host.c_str(), port_text.c_str(), &hints, &results);
  if (rc != 0) {
    if (error) *error = "getaddrinfo failed for " + host + ":" + port_text;
    return TcpSocket();
  }

  TcpSocket socket;
  for (addrinfo* entry = results; entry != nullptr; entry = entry->ai_next) {
    native_socket fd = ::socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
    if (fd == static_cast<native_socket>(-1)) continue;

    if (::connect(fd, entry->ai_addr, static_cast<int>(entry->ai_addrlen)) == 0) {
      int no_delay = 1;
      ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
                   reinterpret_cast<const char*>(&no_delay), sizeof(no_delay));
      socket.fd_ = from_native(fd);
      break;
    }
    RC_CLOSE_SOCKET(fd);
  }

  ::freeaddrinfo(results);

  if (!socket.valid() && error) {
    *error = "connect failed for " + host + ":" + port_text;
  }
  return socket;
}

void TcpListener::close() {
  if (fd_ == -1) return;
  RC_CLOSE_SOCKET(to_native(fd_));
  fd_ = -1;
}

TcpListener TcpListener::listen(uint16_t port, std::string* error) {
  net_init();

  native_socket fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd == static_cast<native_socket>(-1)) {
    if (error) *error = "socket() failed";
    return TcpListener();
  }

  int reuse = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = INADDR_ANY;
  address.sin_port = htons(port);

  if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    if (error) *error = "bind failed on port " + std::to_string(port);
    RC_CLOSE_SOCKET(fd);
    return TcpListener();
  }

  if (::listen(fd, 64) != 0) {
    if (error) *error = "listen failed on port " + std::to_string(port);
    RC_CLOSE_SOCKET(fd);
    return TcpListener();
  }

  TcpListener listener;
  listener.fd_ = from_native(fd);
  return listener;
}

TcpSocket TcpListener::accept() {
  native_socket fd = ::accept(to_native(fd_), nullptr, nullptr);
  if (fd == static_cast<native_socket>(-1)) return TcpSocket();
  int no_delay = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
               reinterpret_cast<const char*>(&no_delay), sizeof(no_delay));
  return TcpSocket(from_native(fd));
}

}  // namespace rc
