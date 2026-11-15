#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace rc {

// ICE 配置：服务器 URL 使用标准格式，凭据内嵌在 URL 中。
//   stun:stun.example.com:3478
//   turn:user:pass@turn.example.com:3478?transport=udp
struct WebRtcConfig {
  std::vector<std::string> ice_servers;
  // 只使用 TURN 中继候选（对应 libdatachannel 的 TransportPolicy::Relay）。
  bool force_relay = false;
  // 允许 ICE over TCP 候选；TURN TCP/TLS 由 URL 的 transport 参数决定。
  bool enable_ice_tcp = false;
};

// WebRTC 传输层（libdatachannel）：只负责 RTP/SCTP，采集、编码、解码仍由现有模块完成。
// 信令（SDP/ICE）由调用方通过中继转发，本层不感知网络连接方式。
//
// 被控端是 offerer：创建 H.264 视频轨（SendOnly）与 "input" DataChannel；
// 主控端是 answerer：收到 offer 后自动生成 answer，收到视频轨后输出完整 Annex-B 帧。
//
// 线程约定：所有回调都在 libdatachannel 的网络线程上触发，调用方不得在回调里做
// 解码/采集等耗时工作（POC 实测会阻塞 RTP 与 DataChannel）。

struct WebRtcSignaling {
  // type 为 "offer" 或 "answer"，sdp 为完整 SDP 文本。
  std::function<void(const std::string& type, const std::string& sdp)> on_local_description;
  std::function<void(const std::string& candidate, const std::string& mid)> on_local_candidate;
};

class WebRtcHost {
 public:
  struct Callbacks {
    WebRtcSignaling signaling;
    // "input" DataChannel 收到的二进制消息，格式为 [MsgType 1 字节][原 TCP 输入载荷]。
    std::function<void(const uint8_t* data, std::size_t size)> on_input;
  };

  WebRtcHost();
  ~WebRtcHost();
  WebRtcHost(const WebRtcHost&) = delete;
  WebRtcHost& operator=(const WebRtcHost&) = delete;

  // 创建 PeerConnection 并按 config 收集候选；offer 由 libdatachannel 自动协商后
  // 通过 signaling.on_local_description 回调送出。
  bool start(const Callbacks& callbacks, const WebRtcConfig& config, std::string* error);

  bool set_answer(const std::string& sdp, std::string* error);
  void add_remote_candidate(const std::string& candidate, const std::string& mid);

  // 发送一帧完整 Annex-B 码流。DTLS-SRTP 未就绪时返回 false，调用方应丢弃本帧继续。
  bool send_video(const uint8_t* data, std::size_t size);

  bool connected() const;
  void close();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class WebRtcClient {
 public:
  struct Callbacks {
    WebRtcSignaling signaling;
    // 解包后的完整 Annex-B 帧；rtp_timestamp 为 RTP 时间戳，仅用于统计。
    std::function<void(const uint8_t* data, std::size_t size, uint32_t rtp_timestamp)>
        on_video;
  };

  WebRtcClient();
  ~WebRtcClient();
  WebRtcClient(const WebRtcClient&) = delete;
  WebRtcClient& operator=(const WebRtcClient&) = delete;

  bool start(const Callbacks& callbacks, const WebRtcConfig& config, std::string* error);

  // 收到 offer 后调用：libdatachannel 会自动生成 answer，并通过回调送出。
  bool set_offer(const std::string& sdp, std::string* error);
  void add_remote_candidate(const std::string& candidate, const std::string& mid);

  // "input" DataChannel 尚未建立或已断开时返回 false。
  bool send_input(const uint8_t* data, std::size_t size);

  bool connected() const;
  void close();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace rc
