#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace rc {

// 跨平台 TCP 封装。Windows 下使用 SOCKET，POSIX 下使用 int。
void net_init();

class TcpSocket {
 public:
  TcpSocket() = default;
  explicit TcpSocket(intptr_t native_fd) : fd_(native_fd) {}

  TcpSocket(TcpSocket&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
  TcpSocket& operator=(TcpSocket&& other) noexcept {
    if (this != &other) {
      close();
      fd_ = other.fd_;
      other.fd_ = -1;
    }
    return *this;
  }

  TcpSocket(const TcpSocket&) = delete;
  TcpSocket& operator=(const TcpSocket&) = delete;

  ~TcpSocket() { close(); }

  bool valid() const { return fd_ != -1; }
  void close();

  bool send_all(const void* data, std::size_t length);
  // 分散写：一次系统调用按序发出多段缓冲（最多 3 段），避免把帧头和载荷
  // 拼成一份临时拷贝。部分写会自动推进游标重发。
  bool send_vectored(const uint8_t* const* buffers, const std::size_t* sizes, int count);
  bool recv_exact(void* data, std::size_t length);
  void set_recv_timeout_ms(int milliseconds);
  // 发送超时：对端长期不读时，send 不能无限阻塞（会连带卡住持锁的调用方）。
  void set_send_timeout_ms(int milliseconds);

  static TcpSocket connect(const std::string& host, uint16_t port, std::string* error);

 private:
  intptr_t fd_ = -1;
};

class TcpListener {
 public:
  TcpListener() = default;

  TcpListener(TcpListener&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
  TcpListener& operator=(TcpListener&& other) noexcept {
    if (this != &other) {
      close();
      fd_ = other.fd_;
      other.fd_ = -1;
    }
    return *this;
  }

  TcpListener(const TcpListener&) = delete;
  TcpListener& operator=(const TcpListener&) = delete;

  ~TcpListener() { close(); }

  bool valid() const { return fd_ != -1; }
  void close();

  static TcpListener listen(uint16_t port, std::string* error);
  TcpSocket accept();

 private:
  intptr_t fd_ = -1;
};

}  // namespace rc
