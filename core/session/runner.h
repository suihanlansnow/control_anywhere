#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include "base/log.h"
#include "capture/capture.h"
#include "codec/encoder.h"
#include "codec/decoder.h"
#include "net/connection.h"
#include "net/socket.h"
#include "protocol/protocol.h"
#include "render/renderer.h"
#include "session/session_config.h"
#include "session/session.h"

namespace rc {

// 主控端采集到的输入事件；坐标已映射到远程视频帧像素。
struct InputEvent {
  MsgType type = MsgType::InputMouseMove;
  int x = 0;
  int y = 0;
  int button = 0;      // 1 左键、2 右键、3 中键、4 后退、5 前进
  int state = 0;       // 鼠标：0 松开、1 按下；键盘另支持 2 自动重复
  int delta_x = 0;     // 滚轮 angleDelta，通常每格 120
  int delta_y = 0;
  int key = 0;         // Qt::Key 整数值，被控端映射为平台键码
};

class HostRunner {
 public:
  explicit HostRunner(const SessionConfig& cfg);
  ~HostRunner();

  void set_log_callback(LogCallback cb) { log_cb_ = std::move(cb); }
  void set_status_callback(StatusCallback cb) { status_cb_ = std::move(cb); }
  void set_progress_callback(ProgressCallback cb) { progress_cb_ = std::move(cb); }
  // 会话建立前的人工确认：参数为 session_id、主控设备号、别名；
  // 返回 false 表示拒绝。回调在工作线程执行，允许阻塞到用户作出选择。
  using ConfirmCallback = std::function<bool(uint64_t session_id,
                                             const std::string& peer_id,
                                             const std::string& peer_alias)>;
  void set_confirm_callback(ConfirmCallback cb) { confirm_cb_ = std::move(cb); }

  // 临时关闭远程键鼠注入，会话保持连接、继续推流，仅允许观看。
  void set_input_enabled(bool enabled);
  bool input_enabled() const { return input_enabled_.load(); }

  int run();
  void stop();

 private:
  void log_(const std::string& level, const std::string& tag, const std::string& msg);
  int stream_frames_(Connection& conn, uint64_t session_id, bool use_webrtc);

  SessionConfig cfg_;
  std::atomic<bool> stop_flag_{false};
  std::atomic<bool> input_enabled_{true};
  std::atomic<int> requested_fps_{0};  // 主控在会话邀请里协商的 max_fps
  // 主控在 ConnectReq.want.transport 里声明的传输方式；每次邀请重置。
  std::string requested_transport_;
  // CapsNegotiate 结果：主控声称的解码能力与本端配置取交集，每次会话重置。
  bool caps_negotiated_ = false;
  std::string negotiated_codec_;
  int negotiated_quality_ = 0;
  std::vector<std::string> negotiated_features_;
  // 中继在 RegisterAck 下发的 ICE 服务器；本地 cfg_ 配置非空时优先使用本地。
  std::vector<std::string> relay_ice_servers_;
  LogCallback log_cb_;
  StatusCallback status_cb_;
  ProgressCallback progress_cb_;
  ConfirmCallback confirm_cb_;

  // 鉴权失败限制：按主控设备号计数并指数退避，另设 60 秒全局失败窗口，
  // 避免攻击者轮换设备号绕过单设备号的锁定。
  struct AuthFailure {
    int count = 0;
    uint64_t locked_until_ms = 0;
  };
  std::mutex auth_mutex_;
  std::map<std::string, AuthFailure> auth_failures_;
  std::deque<uint64_t> recent_failures_ms_;
};

class ClientRunner {
 public:
  explicit ClientRunner(const SessionConfig& cfg);
  ~ClientRunner();

  void set_log_callback(LogCallback cb) { log_cb_ = std::move(cb); }
  void set_status_callback(StatusCallback cb) { status_cb_ = std::move(cb); }
  void set_progress_callback(ProgressCallback cb) { progress_cb_ = std::move(cb); }
  void set_frame_callback(FrameCallback cb) { frame_cb_ = std::move(cb); }

  int run();
  void stop();
  void send_input(const InputEvent& event);
  // 线程安全：把本地文件加入发送队列，鉴权完成后按顺序传给被控端。
  void queue_file(const std::string& path);

 private:
  void log_(const std::string& level, const std::string& tag, const std::string& msg);

  SessionConfig cfg_;
  std::atomic<bool> stop_flag_{false};
  // 被控端在 VideoConfig 里下发的实际编码；解码器按它创建。
  std::string video_codec_;
  // 中继在 RegisterAck 下发的 ICE 服务器；本地 cfg_ 配置非空时优先使用本地。
  std::vector<std::string> relay_ice_servers_;
  LogCallback log_cb_;
  StatusCallback status_cb_;
  ProgressCallback progress_cb_;
  FrameCallback frame_cb_;

  std::mutex input_mutex_;
  std::condition_variable input_cv_;
  std::deque<InputEvent> input_queue_;
  std::atomic<bool> input_stop_{false};
  std::atomic<uint64_t> input_session_{0};

  std::mutex file_mutex_;
  std::condition_variable file_cv_;
  std::deque<std::string> file_queue_;
};

}  // namespace rc
