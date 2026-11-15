#include "net/socket.h"

#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>

#include "base/log.h"

#if RC_ENABLE_TLS
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using native_socket = SOCKET;
#define RC_CLOSE_SOCKET closesocket
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
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

bool set_nonblocking(native_socket fd, bool enabled) {
#ifdef _WIN32
  u_long mode = enabled ? 1 : 0;
  return ::ioctlsocket(fd, FIONBIO, &mode) == 0;
#else
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0) return false;
  const int next = enabled ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
  return ::fcntl(fd, F_SETFL, next) == 0;
#endif
}

// 等待非阻塞 connect 完成；返回 true 表示连接成功。
bool wait_connect(native_socket fd, int timeout_ms) {
  fd_set writable;
  fd_set failed;
  FD_ZERO(&writable);
  FD_ZERO(&failed);
  FD_SET(fd, &writable);
  FD_SET(fd, &failed);

  timeval tv{};
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;

#ifdef _WIN32
  const int rc = ::select(0, nullptr, &writable, &failed, &tv);
#else
  const int rc = ::select(fd + 1, nullptr, &writable, &failed, &tv);
#endif
  if (rc <= 0) return false;

  int socket_error = 0;
#ifdef _WIN32
  int length = sizeof(socket_error);
#else
  socklen_t length = sizeof(socket_error);
#endif
  if (::getsockopt(fd, SOL_SOCKET, SO_ERROR,
                   reinterpret_cast<char*>(&socket_error), &length) != 0) {
    return false;
  }
  return socket_error == 0;
}

}  // namespace

namespace {

#if RC_ENABLE_TLS
int socket_last_error() {
#ifdef _WIN32
  return WSAGetLastError();
#else
  return errno;
#endif
}

// 阻塞 socket 上的超时/中断统一视为 "稍后重试"，由上层按帧超时处理。
bool socket_would_block(int error) {
#ifdef _WIN32
  return error == WSAEWOULDBLOCK || error == WSAETIMEDOUT || error == WSAEINTR;
#else
  return error == EAGAIN || error == EWOULDBLOCK || error == EINTR;
#endif
}

std::string normalize_fingerprint(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (const char ch : text) {
    if (ch == ':' || ch == ' ' || ch == '-') continue;
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  return out;
}

std::string sha256_hex(const unsigned char* data, std::size_t size) {
  unsigned char digest[32] = {};
  if (mbedtls_sha256(data, size, digest, 0) != 0) return {};
  static const char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(64);
  for (const unsigned char byte : digest) {
    out.push_back(kHex[byte >> 4]);
    out.push_back(kHex[byte & 0x0F]);
  }
  return out;
}

void set_tls_error(std::string* error, const std::string& stage, int code) {
  char text[160] = {};
  mbedtls_strerror(code, text, sizeof(text));
  if (error) *error = stage + ": " + text;
}
#endif

}  // namespace

struct TcpSocket::TlsImpl {
  TlsImpl() {
#if RC_ENABLE_TLS
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);
    mbedtls_x509_crt_init(&cert);
    mbedtls_x509_crt_init(&ca);
    mbedtls_pk_init(&pkey);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);
#endif
  }

  ~TlsImpl() {
#if RC_ENABLE_TLS
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    mbedtls_x509_crt_free(&cert);
    mbedtls_x509_crt_free(&ca);
    mbedtls_pk_free(&pkey);
    mbedtls_entropy_free(&entropy);
    mbedtls_ctr_drbg_free(&ctr_drbg);
#endif
  }

#if RC_ENABLE_TLS
  static int bio_send(void* ctx, const unsigned char* buf, std::size_t len) {
    auto* impl = static_cast<TlsImpl*>(ctx);
    const int sent = static_cast<int>(
        ::send(static_cast<native_socket>(impl->fd),
               reinterpret_cast<const char*>(buf), static_cast<int>(len), 0));
    if (sent >= 0) return sent;
    if (socket_would_block(socket_last_error())) return MBEDTLS_ERR_SSL_WANT_WRITE;
    return MBEDTLS_ERR_NET_SEND_FAILED;
  }

  static int bio_recv(void* ctx, unsigned char* buf, std::size_t len) {
    auto* impl = static_cast<TlsImpl*>(ctx);
    const int got = static_cast<int>(
        ::recv(static_cast<native_socket>(impl->fd),
               reinterpret_cast<char*>(buf), static_cast<int>(len), 0));
    if (got > 0) return got;
    if (got == 0) return 0;  // 对端关闭
    if (socket_would_block(socket_last_error())) return MBEDTLS_ERR_SSL_WANT_READ;
    return MBEDTLS_ERR_NET_RECV_FAILED;
  }

  // 阻塞 BIO 只有在 socket 超时时才会返回 WANT_*，因此这里只需限制总时长。
  static int handshake(TlsImpl& impl) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    for (;;) {
      const int rc = mbedtls_ssl_handshake(&impl.ssl);
      if (rc == 0) return 0;
      if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
        if (std::chrono::steady_clock::now() >= deadline) return MBEDTLS_ERR_SSL_TIMEOUT;
        continue;
      }
      return rc;
    }
  }

  mbedtls_ssl_context ssl{};
  mbedtls_ssl_config conf{};
  mbedtls_x509_crt cert;
  mbedtls_x509_crt ca;
  mbedtls_pk_context pkey{};
  mbedtls_entropy_context entropy{};
  mbedtls_ctr_drbg_context ctr_drbg{};
  bool ssl_ready = false;
  bool fingerprint_verified = false;
#endif
  intptr_t fd = -1;
  std::string fingerprint;
};

TcpSocket::TcpSocket() = default;

TcpSocket::TcpSocket(intptr_t native_fd) : fd_(native_fd) {}

TcpSocket::TcpSocket(TcpSocket&& other) noexcept
    : fd_(other.fd_), tls_(std::move(other.tls_)) {
  other.fd_ = -1;
}

TcpSocket& TcpSocket::operator=(TcpSocket&& other) noexcept {
  if (this != &other) {
    close();
    fd_ = other.fd_;
    tls_ = std::move(other.tls_);
    other.fd_ = -1;
  }
  return *this;
}

TcpSocket::~TcpSocket() { close(); }

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
  tls_.reset();
  if (fd_ == -1) return;
  RC_CLOSE_SOCKET(to_native(fd_));
  fd_ = -1;
}

std::string TcpSocket::tls_peer_fingerprint() const {
  return tls_ ? tls_->fingerprint : std::string();
}

bool TcpSocket::start_tls_server(const std::string& cert_path,
                                 const std::string& key_path, std::string* error) {
#if !RC_ENABLE_TLS
  (void)cert_path;
  (void)key_path;
  if (error) *error = "TLS support is not compiled in";
  return false;
#else
  if (!valid()) {
    if (error) *error = "socket is not connected";
    return false;
  }
  static std::once_flag psa_once;
  std::call_once(psa_once, [] { psa_crypto_init(); });

  auto impl = std::make_unique<TlsImpl>();
  impl->fd = fd_;

  int rc = mbedtls_ctr_drbg_seed(
      &impl->ctr_drbg, mbedtls_entropy_func, &impl->entropy,
      reinterpret_cast<const unsigned char*>("rc-tls"), 6);
  if (rc != 0) {
    set_tls_error(error, "drbg seed", rc);
    return false;
  }
  if ((rc = mbedtls_x509_crt_parse_file(&impl->cert, cert_path.c_str())) != 0) {
    set_tls_error(error, "load certificate " + cert_path, rc);
    return false;
  }
  if ((rc = mbedtls_pk_parse_keyfile(&impl->pkey, key_path.c_str(), nullptr,
                                     mbedtls_ctr_drbg_random,
                                     &impl->ctr_drbg)) != 0) {
    set_tls_error(error, "load private key " + key_path, rc);
    return false;
  }
  rc = mbedtls_ssl_config_defaults(&impl->conf, MBEDTLS_SSL_IS_SERVER,
                                   MBEDTLS_SSL_TRANSPORT_STREAM,
                                   MBEDTLS_SSL_PRESET_DEFAULT);
  if (rc != 0) {
    set_tls_error(error, "ssl config", rc);
    return false;
  }
  mbedtls_ssl_conf_rng(&impl->conf, mbedtls_ctr_drbg_random, &impl->ctr_drbg);
  mbedtls_ssl_conf_own_cert(&impl->conf, &impl->cert, &impl->pkey);
  // 客户端身份由上层密码挑战校验，这里不要求客户端证书。
  mbedtls_ssl_conf_authmode(&impl->conf, MBEDTLS_SSL_VERIFY_NONE);
  if ((rc = mbedtls_ssl_setup(&impl->ssl, &impl->conf)) != 0) {
    set_tls_error(error, "ssl setup", rc);
    return false;
  }
  mbedtls_ssl_set_bio(&impl->ssl, impl.get(), &TlsImpl::bio_send,
                      &TlsImpl::bio_recv, nullptr);

  set_recv_timeout_ms(10000);
  set_send_timeout_ms(10000);
  rc = TlsImpl::handshake(*impl);
  set_recv_timeout_ms(0);
  set_send_timeout_ms(0);
  if (rc != 0) {
    set_tls_error(error, "handshake", rc);
    return false;
  }
  impl->ssl_ready = true;
  tls_ = std::move(impl);
  return true;
#endif
}

bool TcpSocket::start_tls_client(const std::string& server_name,
                                 const std::string& ca_path,
                                 const std::string& fingerprint_sha256, bool insecure,
                                 std::string* error) {
#if !RC_ENABLE_TLS
  (void)server_name;
  (void)ca_path;
  (void)fingerprint_sha256;
  (void)insecure;
  if (error) *error = "TLS support is not compiled in";
  return false;
#else
  if (!valid()) {
    if (error) *error = "socket is not connected";
    return false;
  }
  if (!insecure && ca_path.empty() && fingerprint_sha256.empty()) {
    if (error) {
      *error = "TLS needs a CA file, a certificate fingerprint, or --tls-insecure";
    }
    return false;
  }
  static std::once_flag psa_once;
  std::call_once(psa_once, [] { psa_crypto_init(); });

  auto impl = std::make_unique<TlsImpl>();
  impl->fd = fd_;

  int rc = mbedtls_ctr_drbg_seed(
      &impl->ctr_drbg, mbedtls_entropy_func, &impl->entropy,
      reinterpret_cast<const unsigned char*>("rc-tls"), 6);
  if (rc != 0) {
    set_tls_error(error, "drbg seed", rc);
    return false;
  }
  rc = mbedtls_ssl_config_defaults(&impl->conf, MBEDTLS_SSL_IS_CLIENT,
                                   MBEDTLS_SSL_TRANSPORT_STREAM,
                                   MBEDTLS_SSL_PRESET_DEFAULT);
  if (rc != 0) {
    set_tls_error(error, "ssl config", rc);
    return false;
  }
  mbedtls_ssl_conf_rng(&impl->conf, mbedtls_ctr_drbg_random, &impl->ctr_drbg);

  if (!ca_path.empty()) {
    if ((rc = mbedtls_x509_crt_parse_file(&impl->ca, ca_path.c_str())) != 0) {
      set_tls_error(error, "load CA " + ca_path, rc);
      return false;
    }
    mbedtls_ssl_conf_ca_chain(&impl->conf, &impl->ca, nullptr);
    mbedtls_ssl_conf_authmode(&impl->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
  } else {
    mbedtls_ssl_conf_authmode(&impl->conf, MBEDTLS_SSL_VERIFY_NONE);
  }

  if ((rc = mbedtls_ssl_setup(&impl->ssl, &impl->conf)) != 0) {
    set_tls_error(error, "ssl setup", rc);
    return false;
  }
  const std::string host = server_name.empty() ? "relay" : server_name;
  if ((rc = mbedtls_ssl_set_hostname(&impl->ssl, host.c_str())) != 0) {
    set_tls_error(error, "set hostname", rc);
    return false;
  }
  mbedtls_ssl_set_bio(&impl->ssl, impl.get(), &TlsImpl::bio_send,
                      &TlsImpl::bio_recv, nullptr);

  set_recv_timeout_ms(10000);
  set_send_timeout_ms(10000);
  rc = TlsImpl::handshake(*impl);
  set_recv_timeout_ms(0);
  set_send_timeout_ms(0);
  if (rc != 0) {
    set_tls_error(error, "handshake", rc);
    return false;
  }

  const mbedtls_x509_crt* peer = mbedtls_ssl_get_peer_cert(&impl->ssl);
  if (!peer) {
    if (error) *error = "peer did not present a certificate";
    return false;
  }
  impl->fingerprint = sha256_hex(peer->raw.p, peer->raw.len);
  if (!fingerprint_sha256.empty() &&
      normalize_fingerprint(fingerprint_sha256) != impl->fingerprint) {
    if (error) {
      *error = "certificate fingerprint mismatch: got " + impl->fingerprint;
    }
    return false;
  }
  impl->ssl_ready = true;
  tls_ = std::move(impl);
  return true;
#endif
}

bool TcpSocket::send_all(const void* data, std::size_t length) {
#if RC_ENABLE_TLS
  if (tls_) {
    const unsigned char* cursor = static_cast<const unsigned char*>(data);
    std::size_t remaining = length;
    while (remaining > 0) {
      const int sent = mbedtls_ssl_write(&tls_->ssl, cursor,
                                         static_cast<std::size_t>(remaining));
      if (sent > 0) {
        cursor += sent;
        remaining -= static_cast<std::size_t>(sent);
        continue;
      }
      // 阻塞 socket 上 WANT_* 只会在发送超时出现，直接按失败处理。
      return false;
    }
    return true;
  }
#endif
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
#if RC_ENABLE_TLS
  // TLS 没有分散写；逐段加密发送，仍只做一次内存读。
  if (tls_) {
    if (count <= 0) return false;
    for (int i = 0; i < count; ++i) {
      if (sizes[i] == 0) continue;
      if (!send_all(buffers[i], sizes[i])) return false;
    }
    return true;
  }
#endif
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

bool TcpSocket::recv_exact(void* data, std::size_t length, bool* timed_out) {
#if RC_ENABLE_TLS
  if (tls_) {
    unsigned char* cursor = static_cast<unsigned char*>(data);
    std::size_t remaining = length;
    while (remaining > 0) {
      const int got = mbedtls_ssl_read(&tls_->ssl, cursor, remaining);
      if (got > 0) {
        cursor += got;
        remaining -= static_cast<std::size_t>(got);
        continue;
      }
      if (got == 0 || got == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
        if (timed_out) *timed_out = false;
        return false;
      }
      if (got == MBEDTLS_ERR_SSL_WANT_READ || got == MBEDTLS_ERR_SSL_WANT_WRITE) {
        if (timed_out) *timed_out = true;
        return false;
      }
      if (timed_out) *timed_out = false;
      return false;
    }
    if (timed_out) *timed_out = false;
    return true;
  }
#endif
  char* cursor = static_cast<char*>(data);
  std::size_t remaining = length;
  while (remaining > 0) {
    const int got = static_cast<int>(
        ::recv(to_native(fd_), cursor, static_cast<int>(remaining), 0));
    if (got <= 0) {
      if (timed_out) {
#ifdef _WIN32
        *timed_out = (got < 0 && WSAGetLastError() == WSAETIMEDOUT);
#else
        *timed_out = (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
#endif
      }
      return false;
    }
    cursor += got;
    remaining -= static_cast<std::size_t>(got);
  }
  if (timed_out) *timed_out = false;
  return true;
}

int TcpSocket::recv_some(void* data, std::size_t length) {
  if (fd_ == -1 || length == 0) return -1;
#if RC_ENABLE_TLS
  if (tls_) {
    const int got = mbedtls_ssl_read(&tls_->ssl, static_cast<unsigned char*>(data),
                                     length);
    return got > 0 ? got : -1;
  }
#endif
  return static_cast<int>(
      ::recv(to_native(fd_), static_cast<char*>(data), static_cast<int>(length), 0));
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
                             std::string* error, int timeout_ms,
                             bool no_delay) {
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

    bool connected = false;
    if (timeout_ms <= 0) {
      connected = ::connect(fd, entry->ai_addr, static_cast<int>(entry->ai_addrlen)) == 0;
    } else if (set_nonblocking(fd, true)) {
      // 非阻塞 connect + select，避免对端不可达时卡在内核默认超时（可达 20s+）。
      const int started = ::connect(fd, entry->ai_addr, static_cast<int>(entry->ai_addrlen));
      if (started == 0) {
        connected = true;
      } else {
#ifdef _WIN32
        const bool in_progress = ::WSAGetLastError() == WSAEWOULDBLOCK;
#else
        const bool in_progress = errno == EINPROGRESS;
#endif
        connected = in_progress && wait_connect(fd, timeout_ms);
      }
      set_nonblocking(fd, false);
    } else {
      connected = ::connect(fd, entry->ai_addr, static_cast<int>(entry->ai_addrlen)) == 0;
    }

    if (connected) {
      if (no_delay) {
        int value = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
                     reinterpret_cast<const char*>(&value), sizeof(value));
      }
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
  return listen(std::string(), port, error);
}

TcpListener TcpListener::listen(const std::string& bind_address, uint16_t port,
                                std::string* error) {
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
  address.sin_port = htons(port);
  if (bind_address.empty() || bind_address == "*" || bind_address == "0.0.0.0") {
    address.sin_addr.s_addr = INADDR_ANY;
  } else {
    const std::string text = bind_address == "localhost" ? "127.0.0.1" : bind_address;
    if (::inet_pton(AF_INET, text.c_str(), &address.sin_addr) != 1) {
      if (error) *error = "invalid bind address: " + bind_address;
      RC_CLOSE_SOCKET(fd);
      return TcpListener();
    }
  }

  if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    if (error) {
      *error = "bind failed on " +
               (bind_address.empty() ? std::string("0.0.0.0") : bind_address) + ":" +
               std::to_string(port);
    }
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
