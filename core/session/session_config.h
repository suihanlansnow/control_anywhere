#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace rc {

struct SessionConfig {
  std::string relay_host = "127.0.0.1";
  uint16_t relay_port = 21116;
  std::string device_id;
  std::string alias;
  std::string target_id;
  std::string password;
  std::string codec = "h264";
  std::string capture;
  // 视频传输方式："tcp"（默认，走中继的 24 字节帧）或 "webrtc"（RTP/SCTP，
  // 信令仍复用同一条中继连接）。主控端声明，被控端按需创建 PeerConnection。
  std::string transport = "tcp";
  // ICE 服务器（标准 URL：stun:host:port / turn:user:pass@host:port?transport=udp）。
  // 留空时使用中继在 RegisterAck 下发的配置。
  std::vector<std::string> ice_servers;
  bool ice_force_relay = false;  // 只走 TURN
  bool ice_tcp = false;          // 允许 ICE over TCP 候选
  // 双向同步纯文本剪贴板（默认关闭，需主控端权限页显式开启）。
  bool clipboard = false;
  // 允许接收对端发送的文件（默认关闭，文件落到 Downloads/rc-recv）。
  bool file_transfer = false;
  // 与中继之间启用 TLS。证书校验三选一：ca 文件、SHA-256 指纹、或显式不安全模式。
  bool tls = false;
  std::string tls_ca;
  std::string tls_fingerprint;
  // 证书主机名；留空时按 "relay" 校验（自签证书通常配指纹固定）。
  std::string tls_server_name;
  bool tls_insecure = false;
  int fps = 60;
  int quality = 75;
  int monitor = 0;
  int max_frames = 0;
  // 连接中继的超时（毫秒），<=0 表示使用系统默认。
  int connect_timeout_ms = 5000;
  // 默认开启 TCP_NODELAY，交互式鼠标/键盘对延迟敏感。
  bool tcp_no_delay = true;
  bool render = true;
};

using LogCallback = std::function<void(const std::string& tag, const std::string& level, const std::string& msg)>;
using StatusCallback = std::function<void(const std::string& state)>;
// 字节数用 64 位：长时间会话累计会超过 2GB，int 会溢出成负数。
using ProgressCallback = std::function<void(int64_t sent, int64_t bytes_total)>;
// 解码后的 BGRA 画面回调（主控端渲染/显示用），只在回调期间有效，调用方需自行拷贝。
using FrameCallback = std::function<void(const uint8_t* bgra, int width, int height,
                                         int stride)>;

}  // namespace rc
