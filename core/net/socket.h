#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace rc {

// 跨平台 TCP 封装。Windows 下使用 SOCKET，POSIX 下使用 int。
void net_init();

class TcpSocket {
 public:
  TcpSocket();
  explicit TcpSocket(intptr_t native_fd);

  TcpSocket(TcpSocket&& other) noexcept;
  TcpSocket& operator=(TcpSocket&& other) noexcept;

  TcpSocket(const TcpSocket&) = delete;
  TcpSocket& operator=(const TcpSocket&) = delete;

  ~TcpSocket();

  bool valid() const { return fd_ != -1; }
  void close();

  // 在已连通的 TCP 上完成 TLS 握手，之后所有读写自动走加密通道。
  // cert/key 为 PEM 文件；server_name 用于证书主机名校验。
  // 客户端校验方式：ca_path 非空则按 CA 校验；否则若给 fingerprint_sha256
  // 则固定证书指纹；insecure=true 表示不校验（仅限本机/测试）。
  bool start_tls_server(const std::string& cert_path, const std::string& key_path,
                        std::string* error);
  bool start_tls_client(const std::string& server_name, const std::string& ca_path,
                        const std::string& fingerprint_sha256, bool insecure,
                        std::string* error);
  bool tls_active() const { return tls_ != nullptr; }
  std::string tls_peer_fingerprint() const;
  // 本地端点地址（IPv4/IPv6 文本）；用于让对端知道“它连的是本机哪个地址”。
  std::string local_address() const;

  bool send_all(const void* data, std::size_t length);
  // 分散写：一次系统调用按序发出多段缓冲（最多 3 段），避免把帧头和载荷
  // 拼成一份临时拷贝。部分写会自动推进游标重发。
  bool send_vectored(const uint8_t* const* buffers, const std::size_t* sizes, int count);
  // 成功返回 true；对端关闭返回 false 且 timed_out=false；超时返回 false 且
  // timed_out=true（仅 POSIX + SO_RCVTIMEO 设了正超时才有此区分）。
  bool recv_exact(void* data, std::size_t length, bool* timed_out = nullptr);
  // 读取最多 length 字节，返回实际字节数；<=0 表示出错、超时或对端关闭。
  // HTTP 之类按分隔符解析的协议需要“读多少算多少”的语义。
  int recv_some(void* data, std::size_t length);
  void set_recv_timeout_ms(int milliseconds);
  // 发送超时：对端长期不读时，send 不能无限阻塞（会连带卡住持锁的调用方）。
  void set_send_timeout_ms(int milliseconds);

  static TcpSocket connect(const std::string& host, uint16_t port, std::string* error,
                           int timeout_ms = 5000, bool no_delay = true);

 private:
  struct TlsImpl;

  intptr_t fd_ = -1;
  std::unique_ptr<TlsImpl> tls_;
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
  // 绑定到指定地址；空串或 "*" 表示所有网卡，默认仍监听 0.0.0.0。
  static TcpListener listen(const std::string& bind_address, uint16_t port,
                            std::string* error);
  TcpSocket accept();

 private:
  intptr_t fd_ = -1;
};

}  // namespace rc
