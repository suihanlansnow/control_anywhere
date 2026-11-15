#include "session/runner.h"

#include "clipboard/clipboard.h"
#include "transfer/file_transfer.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "base/clock.h"
#include "crypto/hash.h"
#include "net/connection.h"
#include "net/socket.h"
#include "protocol/video_payload.h"
#include "render/renderer.h"
#ifdef __linux__
#include "input/linux_input_injector.h"
#include "render/sdl_renderer.h"
#elif defined(_WIN32)
#include "input/windows_input_injector.h"
#include "render/window_renderer.h"
#endif
#ifdef RC_ENABLE_WEBRTC
#include "webrtc/webrtc_transport.h"
#endif

namespace rc {

#if defined(__linux__)
using PlatformInputInjector = LinuxInputInjector;
static constexpr const char* kInputBackendName = "uinput";
#elif defined(_WIN32)
using PlatformInputInjector = WindowsInputInjector;
static constexpr const char* kInputBackendName = "SendInput";
#endif

static json::Value make_object() { return json::Value::make_object(); }

static bool send_register_cfg(Connection& conn, const SessionConfig& cfg) {
  json::Value request = make_object();
  request.set("device_id", cfg.device_id);
  request.set("alias", cfg.alias.empty() ? cfg.device_id : cfg.alias);
#ifdef __linux__
  request.set("os", "linux");
#else
  request.set("os", "windows");
#endif
  request.set("os_ver", "m0");
  request.set("app", "0.1.0");
  json::Value caps = make_object();
  json::Value codec = json::Value::make_array();
  codec.push(cfg.codec);
  caps.set("codec", codec);
  caps.set("input", true);
  request.set("caps", caps);
  return conn.send_json(MsgType::Register, 0, request);
}

static json::Value parse_payload_cfg(const DecodedFrame& frame) {
  try {
    return json::Value::parse(
        std::string(frame.payload.begin(), frame.payload.end()));
  } catch (const std::exception&) {
    return make_object();
  }
}

// 主控能力列表里优先保留被控端当前编码；对方不支持时退回 jpeg（两端都有实现）。
static std::string pick_codec(const std::string& host_codec, const json::Value* codecs) {
  if (!codecs || !codecs->is_array() || codecs->size() == 0) return host_codec;
  for (std::size_t i = 0; i < codecs->size(); ++i) {
    if (codecs->at(i).as_string() == host_codec) return host_codec;
  }
  for (std::size_t i = 0; i < codecs->size(); ++i) {
    if (codecs->at(i).as_string() == "jpeg") return "jpeg";
  }
  for (std::size_t i = 0; i < codecs->size(); ++i) {
    if (codecs->at(i).as_string() == "h264") return "h264";
  }
  return host_codec;
}

// 本端已实现的可协商特性；未实现的不会回执给主控端。
static bool feature_supported(const std::string& name) {
  return name == "keyframe-request" || name == "video-stats";
}

// 本机剪贴板与对端做纯文本同步：轮询本机变化后发送；写入远端文本后短暂静默，
// 避免 watcher 把刚写进去的内容再发回对端形成回环。
class ClipboardBridge {
 public:
  using SendCallback = std::function<bool(const std::string&)>;
  using LogFn = std::function<void(const std::string& level, const std::string& msg)>;

  ClipboardBridge(SendCallback send, LogFn log) : send_(std::move(send)), log_(std::move(log)) {
    std::string error;
    clipboard_ = create_clipboard(&error);
    if (!clipboard_ && log_) log_("WARN", "clipboard unavailable: " + error);
  }
  ~ClipboardBridge() { stop(); }

  void start() {
    if (!clipboard_ || thread_.joinable()) return;
    stop_.store(false);
    thread_ = std::thread([this] { run(); });
  }

  void stop() {
    stop_.store(true);
    if (thread_.joinable()) thread_.join();
  }

  // 收到远端文本：写本机剪贴板并记录，避免被 watcher 原样发回去。
  void apply_remote(const std::string& text) {
    if (!clipboard_) return;
    if (text.size() > kMaxBytes) {
      if (log_) log_("WARN", "clipboard ignored: " + std::to_string(text.size()) + " bytes");
      return;
    }
    std::string error;
    if (!clipboard_->write_text(text, &error)) {
      if (log_) log_("WARN", "clipboard write failed: " + error);
      return;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      last_seen_ = text;
      // 轮询与写入之间可能读到旧内容，短暂静默避免把旧文本回传。
      suppress_until_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(800);
    }
    if (log_) log_("INFO", "clipboard applied (" + std::to_string(text.size()) + " bytes)");
  }

 private:
  void run() {
    // 先记下当前内容，避免会话一开始就把历史剪贴板当成新内容发出去。
    std::string initial;
    if (clipboard_->read_text(initial, nullptr)) {
      std::lock_guard<std::mutex> lock(mutex_);
      last_seen_ = initial;
    }
    while (!stop_.load()) {
      for (int i = 0; i < 10 && !stop_.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      if (stop_.load()) break;
      std::string text;
      if (!clipboard_->read_text(text, nullptr)) continue;
      if (text.size() > kMaxBytes) continue;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (text == last_seen_) continue;
        if (std::chrono::steady_clock::now() < suppress_until_) continue;
        last_seen_ = text;
      }
      if (!send_(text)) break;
      if (log_) log_("INFO", "clipboard sent (" + std::to_string(text.size()) + " bytes)");
    }
  }

  // 单帧上限 1 MiB，留出 JSON 头部余量。
  static constexpr std::size_t kMaxBytes = 256 * 1024;
  SendCallback send_;
  LogFn log_;
  std::unique_ptr<Clipboard> clipboard_;
  std::mutex mutex_;
  std::string last_seen_;
  std::chrono::steady_clock::time_point suppress_until_{};
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

// 中继在 RegisterAck 里下发的 ICE 配置：{"ice":{"servers":["stun:...","turn:..."]}}。
static std::vector<std::string> parse_ice_servers(const json::Value& ack) {
  std::vector<std::string> servers;
  const json::Value* ice = ack.get("ice");
  if (!ice) return servers;
  const json::Value* list = ice->get("servers");
  if (!list || !list->is_array()) return servers;
  for (std::size_t i = 0; i < list->size(); ++i) {
    const std::string uri = list->at(i).as_string();
    if (!uri.empty()) servers.push_back(uri);
  }
  return servers;
}

void write_input_u16(std::vector<uint8_t>& out, std::size_t offset, uint16_t value) {
  out[offset] = static_cast<uint8_t>(value & 0xff);
  out[offset + 1] = static_cast<uint8_t>(value >> 8);
}

void write_input_u32(std::vector<uint8_t>& out, std::size_t offset, uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    out[offset + i] = static_cast<uint8_t>((value >> (i * 8)) & 0xff);
  }
}

uint16_t read_input_u16(const std::vector<uint8_t>& data, std::size_t offset) {
  return static_cast<uint16_t>(data[offset] | (data[offset + 1] << 8));
}

uint32_t read_input_u32(const std::vector<uint8_t>& data, std::size_t offset) {
  uint32_t value = 0;
  for (int i = 0; i < 4; ++i) {
    value |= static_cast<uint32_t>(data[offset + i]) << (i * 8);
  }
  return value;
}

std::vector<uint8_t> encode_input_event(const InputEvent& event) {
  std::vector<uint8_t> out;
  switch (event.type) {
    case MsgType::InputMouseMove:
      out.resize(10, 0);
      write_input_u32(out, 0, static_cast<uint32_t>(now_ms()));
      write_input_u16(out, 4, static_cast<uint16_t>(std::clamp(event.x, 0, 65535)));
      write_input_u16(out, 6, static_cast<uint16_t>(std::clamp(event.y, 0, 65535)));
      break;
    case MsgType::InputMouseButton:
      out.resize(12, 0);
      write_input_u32(out, 0, static_cast<uint32_t>(now_ms()));
      out[4] = static_cast<uint8_t>(event.button);
      out[5] = static_cast<uint8_t>(event.state != 0);
      write_input_u16(out, 6, static_cast<uint16_t>(std::clamp(event.x, 0, 65535)));
      write_input_u16(out, 8, static_cast<uint16_t>(std::clamp(event.y, 0, 65535)));
      break;
    case MsgType::InputMouseWheel:
      out.resize(8, 0);
      write_input_u32(out, 0, static_cast<uint32_t>(now_ms()));
      write_input_u16(out, 4, static_cast<uint16_t>(static_cast<int16_t>(
          std::clamp(event.delta_x, -32768, 32767))));
      write_input_u16(out, 6, static_cast<uint16_t>(static_cast<int16_t>(
          std::clamp(event.delta_y, -32768, 32767))));
      break;
    case MsgType::InputKey:
      out.resize(12, 0);
      write_input_u32(out, 0, static_cast<uint32_t>(now_ms()));
      write_input_u32(out, 4, static_cast<uint32_t>(event.key));
      out[8] = static_cast<uint8_t>(std::clamp(event.state, 0, 2));
      break;
    default:
      break;
  }
  return out;
}

#if defined(__linux__) || defined(_WIN32)
// TCP 输入消息与 WebRTC DataChannel 共用同一套注入逻辑：
// TCP 载荷从 0 开始，DataChannel 前置 1 字节 MsgType，因此用 offset 区分。
void inject_remote_input(PlatformInputInjector& injector, MsgType type,
                         const std::vector<uint8_t>& payload, std::size_t offset) {
  switch (type) {
    case MsgType::InputMouseMove:
      if (payload.size() >= offset + 10) {
        injector.mouse_move(read_input_u16(payload, offset + 4),
                            read_input_u16(payload, offset + 6));
      }
      break;
    case MsgType::InputMouseButton:
      if (payload.size() >= offset + 12) {
        injector.mouse_button(payload[offset + 4], payload[offset + 5] != 0,
                              read_input_u16(payload, offset + 6),
                              read_input_u16(payload, offset + 8));
      }
      break;
    case MsgType::InputMouseWheel:
      if (payload.size() >= offset + 8) {
        injector.mouse_wheel(
            static_cast<int16_t>(read_input_u16(payload, offset + 4)),
            static_cast<int16_t>(read_input_u16(payload, offset + 6)));
      }
      break;
    case MsgType::InputKey:
      if (payload.size() >= offset + 12) {
        injector.key(static_cast<int>(read_input_u32(payload, offset + 4)),
                     payload[offset + 8]);
      }
      break;
    default:
      break;
  }
}
#endif
HostRunner::HostRunner(const SessionConfig& cfg) : cfg_(cfg) {}
HostRunner::~HostRunner() { stop(); }

void HostRunner::stop() { stop_flag_.store(true); }

void HostRunner::set_input_enabled(bool enabled) {
  if (input_enabled_.exchange(enabled) == enabled) return;
  log_("INFO", "input",
       enabled ? "remote keyboard/mouse enabled" : "remote keyboard/mouse disabled (view only)");
}

void HostRunner::log_(const std::string& level, const std::string& tag, const std::string& msg) {
  if (log_cb_) {
    log_cb_(tag, level, msg);
  } else {
    if (level == "ERROR") RC_LOGE(tag, msg);
    else if (level == "WARN") RC_LOGW(tag, msg);
    else RC_LOGI(tag, msg);
  }
}

int HostRunner::stream_frames_(Connection& conn, uint64_t session_id,
                               bool use_webrtc) {
  std::string error;
  std::unique_ptr<ICaptureSource> capture = create_capture_source(cfg_.capture, &error);
  if (!capture) {
    log_("WARN", "host", error + ", fallback to synthetic");
    capture = create_capture_source("synthetic", &error);
  }
  if (!capture) {
    log_("ERROR", "host", "no capture source available");
    return 0;
  }
  if (!capture->start(cfg_.monitor, &error)) {
    log_("WARN", "host", "capture start failed: " + error + ", fallback to synthetic");
    capture = create_capture_source("synthetic", &error);
    if (!capture || !capture->start(0, &error)) {
      log_("ERROR", "host", "synthetic capture start failed");
      return 0;
    }
  }

  // CapsNegotiate 已按主控能力收敛过；未协商时沿用本端配置。
  const std::string codec = negotiated_codec_.empty() ? cfg_.codec : negotiated_codec_;
  const int quality = negotiated_quality_ > 0 ? negotiated_quality_ : cfg_.quality;

  // B5: H.264 编码器直接吃 NV12，省掉 8MB 回读和 CPU 逐像素转换。
  if (codec == "h264" && !capture->set_output_format(PixelFormat::kNV12)) {
    log_("INFO", "host", "capture cannot output nv12, using bgra");
  }

  std::unique_ptr<IVideoEncoder> encoder = create_encoder(codec, &error);
  if (!encoder) {
    log_("ERROR", "host", "encoder unavailable: " + error);
    return 0;
  }

  // B5: 采集与编码共享同一块 D3D11 设备，硬编零回读。
  if (void* device = capture->native_device(); device != nullptr) {
    if (!encoder->set_d3d_device(device)) {
      log_("INFO", "host", "encoder rejected shared d3d device");
    }
  }

  CapturedFrame probe;
  capture->grab(probe, 300);
  int width = probe.width;
  int height = probe.height;
  // 主控协商的 max_fps 只降不升，避免覆盖被控端自己的上限。
  int fps = cfg_.fps > 0 ? cfg_.fps : 60;
  const int requested_fps = requested_fps_.exchange(0);
  if (requested_fps > 0 && requested_fps < fps) fps = requested_fps;
  if (!encoder->open(width, height, fps, quality, &error)) {
    log_("ERROR", "host", "encoder open failed: " + error);
    return 0;
  }

  // B5: 采集线程是否交 GPU 纹理由编码器 open 后才能确定；两侧都用原子变量同步。
  std::atomic<bool> texture_output{encoder->accepts_texture_input()};
  std::atomic<void*> device_manager{encoder->native_device_manager()};

  log_("INFO", "host",
       std::string("capture=") + capture->name() + " codec=" + encoder->name() + " " +
           std::to_string(width) + "x" + std::to_string(height) + " fps=" + std::to_string(fps));
  if (status_cb_) status_cb_("streaming");

  // 采集线程：队列深度 1 + 覆盖旧帧，保证延迟不累积。
  struct FrameSlot {
    std::mutex mutex;
    std::condition_variable cv;
    CapturedFrame frame;
    bool has_frame = false;
    bool stop = false;
    int captured = 0;
    int overwritten = 0;
  };
  FrameSlot slot;

  // B2+B3: 发送线程：编码节拍不被网络 send 阻塞；队列深度 1 + 覆盖旧帧。
  struct SendTask {
    uint64_t session_id = 0;
    VideoFrameMeta meta;
    std::vector<uint8_t> data;
    uint32_t frame_id = 0;
  };
  struct SendSlot {
    std::mutex mutex;
    std::condition_variable cv;
    SendTask task;
    bool has_frame = false;
    bool stop = false;
  };
  SendSlot send_slot;
  std::atomic<int> sent_count{0};
  std::atomic<uint64_t> sent_bytes{0};
  // 每秒窗口内的编码耗时累计，VideoStats 取平均后清零。
  std::atomic<int64_t> encode_us_sum{0};
  std::atomic<int> encode_samples{0};
  std::atomic<bool> send_failed{false};
  std::atomic<bool> stream_closed{false};
  std::atomic<bool> control_stop{false};

  const int frame_interval_ms = std::max(1, 1000 / fps);

#if defined(__linux__) || defined(_WIN32)
  PlatformInputInjector input_injector;
  std::string input_error;
  if (input_injector.start(width, height, &input_error)) {
    log_("INFO", "input", std::string(kInputBackendName) + " keyboard/mouse ready");
  } else {
    log_("WARN", "input", "remote input unavailable: " + input_error);
  }
#endif

#ifdef RC_ENABLE_WEBRTC
  // WebRTC：视频走 RTP，输入走 DataChannel；信令复用当前中继连接。
  std::unique_ptr<WebRtcHost> rtc;
  std::mutex rtc_input_mutex;
  std::condition_variable rtc_input_cv;
  std::deque<std::vector<uint8_t>> rtc_input_queue;
  std::atomic<bool> rtc_input_stop{false};
  std::atomic<int> rtc_deferred_frames{0};
  std::thread rtc_input_thread;
  if (use_webrtc) {
    rtc = std::make_unique<WebRtcHost>();
    WebRtcHost::Callbacks rtc_cb;
    rtc_cb.signaling.on_local_description = [&conn, session_id](
                                                const std::string& type,
                                                const std::string& sdp) {
      json::Value payload = make_object();
      payload.set("type", type);
      payload.set("sdp", sdp);
      conn.send_json(MsgType::WebRtcOffer, session_id, payload);
    };
    rtc_cb.signaling.on_local_candidate = [&conn, session_id](
                                              const std::string& candidate,
                                              const std::string& mid) {
      json::Value payload = make_object();
      payload.set("candidate", candidate);
      payload.set("mid", mid);
      conn.send_json(MsgType::WebRtcCandidate, session_id, payload);
    };
    rtc_cb.on_input = [&](const uint8_t* data, std::size_t size) {
      if (size < 2) return;
      {
        std::lock_guard<std::mutex> lock(rtc_input_mutex);
        // 输入只保留最新状态，积压时丢最旧的，避免执行一串过期事件。
        if (rtc_input_queue.size() >= 256) rtc_input_queue.pop_front();
        rtc_input_queue.emplace_back(data, data + size);
      }
      rtc_input_cv.notify_one();
    };
    std::string rtc_error;
    WebRtcConfig rtc_config;
    // 本地显式配置优先，否则使用中继在 RegisterAck 下发的 STUN/TURN。
    rtc_config.ice_servers =
        cfg_.ice_servers.empty() ? relay_ice_servers_ : cfg_.ice_servers;
    rtc_config.force_relay = cfg_.ice_force_relay;
    rtc_config.enable_ice_tcp = cfg_.ice_tcp;
    if (!rtc_config.ice_servers.empty()) {
      log_("INFO", "webrtc", "ice servers=" +
                               std::to_string(rtc_config.ice_servers.size()));
    }
    if (!rtc->start(rtc_cb, rtc_config, &rtc_error)) {
      log_("WARN", "webrtc", "start failed: " + rtc_error + ", fallback to tcp");
      rtc.reset();
    } else {
      log_("INFO", "webrtc", "transport ready, offer sent via relay");
    }
  }
#if defined(__linux__) || defined(_WIN32)
  if (rtc) {
    rtc_input_thread = std::thread([&] {
      while (!rtc_input_stop.load()) {
        std::vector<uint8_t> event;
        {
          std::unique_lock<std::mutex> lock(rtc_input_mutex);
          rtc_input_cv.wait(lock, [&] {
            return rtc_input_stop.load() || !rtc_input_queue.empty();
          });
          if (rtc_input_stop.load() && rtc_input_queue.empty()) break;
          event = std::move(rtc_input_queue.front());
          rtc_input_queue.pop_front();
        }
        if (!input_enabled_.load() || event.empty()) continue;
        inject_remote_input(input_injector, static_cast<MsgType>(event[0]), event, 1);
      }
    });
  }
#endif
#else
  (void)use_webrtc;
#endif

  // 会话建立后先下发编码参数：主控端据此显示实际分辨率/帧率，并在需要时请求关键帧。
  std::string active_transport = "tcp";
#ifdef RC_ENABLE_WEBRTC
  if (rtc) active_transport = "webrtc";
#endif
  // 协商回执放在实际分辨率确定之后，主控端一次拿到最终编码参数。
  if (caps_negotiated_) {
    json::Value caps_reply = make_object();
    caps_reply.set("codec", codec);
    caps_reply.set("width", width);
    caps_reply.set("height", height);
    caps_reply.set("fps", fps);
    caps_reply.set("quality", quality);
    json::Value features = json::Value::make_array();
    for (const std::string& name : negotiated_features_) features.push(name);
    caps_reply.set("features", features);
    if (!conn.send_json(MsgType::CapsNegotiate, session_id, caps_reply)) {
      log_("WARN", "host", "send caps reply failed");
    }
  }

  json::Value video_cfg = make_object();
  video_cfg.set("codec", codec);
  video_cfg.set("width", width);
  video_cfg.set("height", height);
  video_cfg.set("fps", fps);
  video_cfg.set("quality", quality);
  const int bitrate_kbps = encoder->target_bitrate_kbps();
  if (bitrate_kbps > 0) video_cfg.set("bitrate_kbps", bitrate_kbps);
  video_cfg.set("keyframe_interval", fps);
  video_cfg.set("transport", active_transport);
  if (conn.send_json(MsgType::VideoConfig, session_id, video_cfg)) {
    log_("INFO", "host",
         "video config: codec=" + codec + " " + std::to_string(width) + "x" +
             std::to_string(height) + " fps=" + std::to_string(fps) + " quality=" +
             std::to_string(quality) +
             (bitrate_kbps > 0 ? " bitrate=" + std::to_string(bitrate_kbps) + "kbps" : "") +
             " transport=" + active_transport);
  } else {
    log_("WARN", "host", "send video config failed");
  }

  // 剪贴板同步默认关闭；开启后由 watcher 线程把本机变化推给主控端。
  std::unique_ptr<ClipboardBridge> clipboard;
  int pending_clipboard_size = -1;
  if (cfg_.clipboard) {
    clipboard = std::make_unique<ClipboardBridge>(
        [&](const std::string& text) {
          json::Value announce = make_object();
          announce.set("format", "text");
          announce.set("size", static_cast<int64_t>(text.size()));
          if (!conn.send_json(MsgType::ClipboardSet, session_id, announce)) return false;
          return conn.send(MsgType::ClipboardData, session_id,
                           reinterpret_cast<const uint8_t*>(text.data()), text.size());
        },
        [&](const std::string& level, const std::string& msg) { log_(level, "clipboard", msg); });
    clipboard->start();
  }

  // 文件接收：一次只处理一个传输，收到 FileOffer 后写入接收目录并回 FileAccept。
  FileReceiver file_receiver;
  uint32_t file_transfer_id = 0;
  bool file_receiving = false;
  auto send_file_accept = [&](uint32_t transfer_id, bool accept, const std::string& reason) {
    json::Value msg = make_object();
    msg.set("transfer_id", static_cast<int64_t>(transfer_id));
    msg.set("accept", accept);
    if (!reason.empty()) msg.set("reason", reason);
    conn.send_json(MsgType::FileAccept, session_id, msg);
  };

  // 推流期间也要接收控制消息，否则主控断开后中继发来的 SessionClose
  // 会一直堆在接收缓冲区，Host 无法结束旧会话或接受下一次连接。
  conn.set_recv_timeout_ms(200);
  std::thread control_thread([&]() {
    while (!control_stop.load() && !stop_flag_.load()) {
      DecodedFrame incoming;
      bool recv_timeout = false;
      if (!conn.recv(incoming, &recv_timeout)) {
        if (recv_timeout) continue;
        stream_closed.store(true);
        break;
      }
      if (incoming.type == MsgType::SessionClose && incoming.session_id == session_id) {
        json::Value ack = make_object();
        ack.set("reason", "ack");
        conn.send_json(MsgType::SessionClose, session_id, ack);
        stream_closed.store(true);
        break;
      }
      if (incoming.session_id != session_id) continue;
#ifdef RC_ENABLE_WEBRTC
      // WebRTC 信令：answer 与 ICE 候选都通过同一条中继连接回传。
      if (rtc) {
        if (incoming.type == MsgType::WebRtcAnswer) {
          json::Value payload = parse_payload_cfg(incoming);
          const json::Value* sdp = payload.get("sdp");
          std::string rtc_error;
          if (!sdp || !rtc->set_answer(sdp->as_string(), &rtc_error)) {
            log_("WARN", "webrtc", "apply answer failed: " + rtc_error);
          } else {
            log_("INFO", "webrtc", "answer applied");
          }
          continue;
        }
        if (incoming.type == MsgType::WebRtcCandidate) {
          json::Value payload = parse_payload_cfg(incoming);
          const json::Value* candidate = payload.get("candidate");
          const json::Value* mid = payload.get("mid");
          if (candidate) {
            rtc->add_remote_candidate(candidate->as_string(),
                                      mid ? mid->as_string() : "video");
          }
          continue;
        }
      }
#endif
      if (clipboard && (incoming.type == MsgType::ClipboardSet ||
                        incoming.type == MsgType::ClipboardData)) {
        if (incoming.type == MsgType::ClipboardSet) {
          json::Value payload = parse_payload_cfg(incoming);
          pending_clipboard_size = payload.get("size")
                                       ? static_cast<int>(payload.get("size")->as_int(-1))
                                       : -1;
        } else {
          if (pending_clipboard_size >= 0 &&
              static_cast<int>(incoming.payload.size()) == pending_clipboard_size) {
            clipboard->apply_remote(
                std::string(incoming.payload.begin(), incoming.payload.end()));
          }
          pending_clipboard_size = -1;
        }
        continue;
      }
      if (incoming.type == MsgType::FileOffer) {
        json::Value payload = parse_payload_cfg(incoming);
        const uint32_t transfer_id =
            payload.get("transfer_id")
                ? static_cast<uint32_t>(payload.get("transfer_id")->as_int(0))
                : 0;
        const std::string name =
            payload.get("name") ? payload.get("name")->as_string("file") : "file";
        const uint64_t size =
            payload.get("size") ? static_cast<uint64_t>(payload.get("size")->as_int(0)) : 0;
        std::string error;
        if (!cfg_.file_transfer) {
          send_file_accept(transfer_id, false, "disabled");
          log_("WARN", "file", "rejected incoming file (file transfer disabled)");
        } else if (file_receiving) {
          send_file_accept(transfer_id, false, "busy");
        } else if (!file_receiver.begin(name, size, &error)) {
          send_file_accept(transfer_id, false, error);
          log_("WARN", "file", "cannot receive file: " + error);
        } else {
          file_transfer_id = transfer_id;
          file_receiving = true;
          send_file_accept(transfer_id, true, "");
          log_("INFO", "file",
               "receiving " + name + " (" + std::to_string(size) + " bytes) -> " +
                   file_receiver.path());
        }
        continue;
      }
      if (incoming.type == MsgType::FileChunk) {
        if (file_receiving && incoming.payload.size() > kFileChunkHeaderBytes) {
          const uint32_t transfer_id = read_u32_le(incoming.payload.data());
          const uint64_t offset = read_u64_le(incoming.payload.data() + 4);
          if (transfer_id == file_transfer_id) {
            std::string error;
            if (!file_receiver.write(offset, incoming.payload.data() + kFileChunkHeaderBytes,
                                     incoming.payload.size() - kFileChunkHeaderBytes, &error)) {
              log_("WARN", "file", "write failed: " + error);
              file_receiver.abort();
              file_receiving = false;
              json::Value abort_msg = make_object();
              abort_msg.set("transfer_id", static_cast<int64_t>(transfer_id));
              abort_msg.set("reason", error);
              conn.send_json(MsgType::FileAbort, session_id, abort_msg);
            }
          }
        }
        continue;
      }
      if (incoming.type == MsgType::FileEnd) {
        if (file_receiving) {
          std::string error;
          const std::string path = file_receiver.path();
          const uint64_t size = file_receiver.expected_size();
          if (file_receiver.finish(&error)) {
            log_("INFO", "file",
                 "file received: " + path + " (" + std::to_string(size) + " bytes)");
          } else {
            log_("WARN", "file", "file incomplete: " + error);
          }
          file_receiving = false;
        }
        continue;
      }
      if (incoming.type == MsgType::FileAbort) {
        if (file_receiving) {
          file_receiver.abort();
          file_receiving = false;
          log_("WARN", "file", "transfer aborted by controller");
        }
        continue;
      }
      if (incoming.type == MsgType::KeyframeReq) {
        json::Value payload = parse_payload_cfg(incoming);
        const std::string reason =
            payload.get("reason") ? payload.get("reason")->as_string("controller")
                                  : "controller";
        if (encoder->request_keyframe()) {
          log_("INFO", "host", "keyframe requested (" + reason + "), forcing next IDR");
        } else {
          log_("WARN", "host", "keyframe request ignored: encoder cannot force IDR");
        }
        continue;
      }
#if defined(__linux__) || defined(_WIN32)
      if (!input_enabled_.load()) {
        // 仅观看模式：消息照常消费，但不下发到注入器。
        continue;
      }
      inject_remote_input(input_injector, incoming.type, incoming.payload, 0);
#endif
    }
  });

  std::thread capture_thread([&, source = std::move(capture)]() mutable {
    while (true) {
      {
        std::lock_guard<std::mutex> lock(slot.mutex);
        if (slot.stop) break;
      }
      if (void* manager = device_manager.load()) {
        source->set_device_manager(manager);
      }
      source->set_texture_output(texture_output.load());
      CapturedFrame frame;
      if (!source->grab(frame, frame_interval_ms)) continue;
      {
        std::lock_guard<std::mutex> lock(slot.mutex);
        if (slot.has_frame) ++slot.overwritten;
        slot.frame = std::move(frame);
        slot.has_frame = true;
        ++slot.captured;
      }
      slot.cv.notify_one();
    }
    source->stop();
  });

  std::thread send_thread([&]() {
    auto last_heartbeat = std::chrono::steady_clock::now();
    // 每秒上报一次累计计数与区间速率，主控端据此显示带宽/帧率/编码耗时。
    auto last_stats = std::chrono::steady_clock::now();
    int stats_frames = 0;
    uint64_t stats_bytes = 0;
    while (true) {
      {
        const auto stats_now = std::chrono::steady_clock::now();
        const int64_t elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       stats_now - last_stats)
                                       .count();
        if (elapsed_ms >= 1000) {
          const int frames_now = sent_count.load();
          const int64_t bytes_now = static_cast<int64_t>(sent_bytes.load());
          int captured_now = 0;
          int dropped_now = 0;
          {
            std::lock_guard<std::mutex> lock(slot.mutex);
            captured_now = slot.captured;
            dropped_now = slot.overwritten;
          }
          const int samples = encode_samples.exchange(0);
          const int64_t encode_us = encode_us_sum.exchange(0);
          json::Value stats = make_object();
          stats.set("frames_sent", frames_now);
          stats.set("bytes_sent", bytes_now);
          stats.set("fps", static_cast<int>((frames_now - stats_frames) * 1000 / elapsed_ms));
          stats.set("bitrate_kbps",
                    static_cast<int>((bytes_now - static_cast<int64_t>(stats_bytes)) * 8 /
                                     elapsed_ms));
          stats.set("captured", captured_now);
          stats.set("dropped", dropped_now);
          stats.set("encode_ms",
                    samples > 0 ? static_cast<int>(encode_us / samples / 1000) : 0);
          stats.set("transport", active_transport);
          if (!conn.send_json(MsgType::VideoStats, session_id, stats)) {
            log_("WARN", "host", "send video stats failed");
            send_failed.store(true);
            break;
          }
          last_stats = stats_now;
          stats_frames = frames_now;
          stats_bytes = static_cast<uint64_t>(bytes_now);
        }
      }
      SendTask task;
      bool has_task = false;
      {
        std::unique_lock<std::mutex> lock(send_slot.mutex);
        send_slot.cv.wait_for(lock, std::chrono::seconds(1), [&] {
          return send_slot.has_frame || send_slot.stop;
        });
        if (send_slot.stop && !send_slot.has_frame) break;
        if (send_slot.has_frame) {
          task = std::move(send_slot.task);
          send_slot.has_frame = false;
          has_task = true;
        }
      }
      if (!has_task) {
        // 空闲 1 秒没有新帧（画面静止/采集无更新）就发心跳，
        // 避免主控端读超时把"没画面"误判成"断线"。
        if (!conn.send(MsgType::Heartbeat, session_id, nullptr, 0)) {
          log_("WARN", "host", "send heartbeat failed");
          send_failed.store(true);
          break;
        }
        continue;
      }
      // B2: 使用 send_video_frame（复用缓冲 + 一次发送），取代 build_video_payload + conn.send。
      bool frame_sent = false;
#ifdef RC_ENABLE_WEBRTC
      if (rtc) {
        // 未连接时 send_video 返回 false：丢弃本帧等链路就绪，不终止会话。
        frame_sent = rtc->send_video(task.data.data(), task.data.size());
        if (!frame_sent) {
          const int deferred = rtc_deferred_frames.fetch_add(1) + 1;
          if (deferred == 1 || deferred % 100 == 0) {
            log_("INFO", "webrtc", "video not connected yet, dropped " +
                                     std::to_string(deferred) + " frames");
          }
          continue;
        }
        // WebRTC 模式下控制连接没有视频帧，需独立保活，否则主控端会读超时。
        const auto now_tp = std::chrono::steady_clock::now();
        if (now_tp - last_heartbeat >= std::chrono::seconds(1)) {
          last_heartbeat = now_tp;
          if (!conn.send(MsgType::Heartbeat, session_id, nullptr, 0)) {
            log_("WARN", "host", "send heartbeat failed");
            send_failed.store(true);
            break;
          }
        }
      } else
#endif
      {
        frame_sent = conn.send_video_frame(task.session_id, task.meta,
                                           task.data.data(), task.data.size(),
                                           task.frame_id);
      }
      if (!frame_sent) {
        log_("WARN", "host", "send frame failed");
        send_failed.store(true);
        break;
      }
      sent_bytes.fetch_add(task.data.size());
      sent_count.fetch_add(1);
    }
  });

  int encoded_count = 0;
  uint32_t frame_id = 0;
  auto next_slot = std::chrono::steady_clock::now();
  auto frame_interval = std::chrono::milliseconds(frame_interval_ms);
  CapturedFrame last_frame;
  bool has_last = false;

  while (!stop_flag_.load() && !send_failed.load() && !stream_closed.load()) {
    bool fresh = false;
    {
      std::unique_lock<std::mutex> lock(slot.mutex);
      // 等到节拍点，期间有新帧就提前取走（队列深度 1，后来的覆盖先来的）。
      slot.cv.wait_until(lock, next_slot, [&slot] { return slot.has_frame || slot.stop; });
      if (slot.stop) { stop_flag_.store(true); break; }
      if (slot.has_frame) {
        last_frame = std::move(slot.frame);
        slot.has_frame = false;
        has_last = true;
        fresh = true;
      }
    }
    next_slot += frame_interval;
    if (next_slot < std::chrono::steady_clock::now()) {
      next_slot = std::chrono::steady_clock::now();
    }

    if (!has_last) continue;
    if (!fresh) {
      // 静止画面没有新帧时复用上一帧，维持目标帧率和持续视频流。
      last_frame.capture_ts_us = static_cast<int64_t>(now_us());
    }

    EncodedFrame encoded;
    std::string enc_error;
    const uint64_t encode_start_us = now_us();
    if (!encoder->encode(last_frame, encoded, &enc_error)) {
      log_("WARN", "host", "encode failed: " + enc_error);
      continue;
    }
    encode_us_sum.fetch_add(static_cast<int64_t>(now_us() - encode_start_us));
    encode_samples.fetch_add(1);

    // B3: 每次 encode 后同步 texture_output / device_manager，允许中途切换。
    device_manager.store(encoder->native_device_manager());
    texture_output.store(encoder->accepts_texture_input());

    ++frame_id;
    VideoFrameMeta meta;
    meta.capture_ts_us = encoded.capture_ts_us;
    meta.encode_ts_us = encoded.encode_ts_us;
    meta.frame_id = frame_id;
    meta.width = static_cast<uint16_t>(encoded.width);
    meta.height = static_cast<uint16_t>(encoded.height);
    meta.codec = static_cast<uint8_t>(encoded.codec);
    meta.keyframe = encoded.keyframe;

    // 推送到发送线程，编码节拍不被网络 send 阻塞。
    {
      std::lock_guard<std::mutex> lock(send_slot.mutex);
      send_slot.task.session_id = session_id;
      send_slot.task.meta = meta;
      send_slot.task.data = std::move(encoded.data);
      send_slot.task.frame_id = frame_id;
      send_slot.has_frame = true;
    }
    send_slot.cv.notify_one();

    ++encoded_count;
    // 进度上报已发送的帧数与字节数（发送线程更新），不再恒为 0。
    if (progress_cb_) {
      progress_cb_(sent_count.load(), static_cast<int64_t>(sent_bytes.load()));
    }
    if (cfg_.max_frames > 0 && encoded_count >= cfg_.max_frames) break;
  }

  {
    std::lock_guard<std::mutex> lock(slot.mutex);
    slot.stop = true;
  }
  slot.cv.notify_all();
  capture_thread.join();

  {
    std::lock_guard<std::mutex> lock(send_slot.mutex);
    send_slot.stop = true;
  }
  send_slot.cv.notify_all();
  send_thread.join();

  control_stop.store(true);
  control_thread.join();
  conn.set_recv_timeout_ms(30000);

#ifdef RC_ENABLE_WEBRTC
  rtc_input_stop.store(true);
  rtc_input_cv.notify_all();
  if (rtc_input_thread.joinable()) rtc_input_thread.join();
  if (rtc) {
    if (rtc_deferred_frames.load() > 0) {
      log_("INFO", "webrtc", "frames dropped before connect=" +
                                  std::to_string(rtc_deferred_frames.load()));
    }
    rtc->close();
  }
#endif

  encoder->close();
  log_("INFO", "host", "frames_encoded=" + std::to_string(encoded_count) +
                         " frames_sent=" + std::to_string(sent_count.load()) +
                         " captured=" + std::to_string(slot.captured) +
                         " dropped=" + std::to_string(slot.overwritten));
  return encoded_count;
}

int HostRunner::run() {
  std::string error;
  TcpSocket socket = TcpSocket::connect(cfg_.relay_host, cfg_.relay_port, &error,
                                        cfg_.connect_timeout_ms, cfg_.tcp_no_delay);
  if (!socket.valid()) {
    log_("ERROR", "host", "relay connect failed: " + error);
    return 2;
  }
  if (cfg_.tls) {
    std::string tls_error;
    if (!socket.start_tls_client(cfg_.tls_server_name, cfg_.tls_ca,
                                 cfg_.tls_fingerprint, cfg_.tls_insecure,
                                 &tls_error)) {
      log_("ERROR", "host", "relay TLS failed: " + tls_error);
      return 2;
    }
    log_("INFO", "host", "relay TLS established" +
                             (socket.tls_peer_fingerprint().empty()
                                  ? std::string()
                                  : ", cert sha256=" + socket.tls_peer_fingerprint()));
  }
  log_("INFO", "host", "connected to relay " + cfg_.relay_host + ":" + std::to_string(cfg_.relay_port));
  if (status_cb_) status_cb_("connected");

  Connection conn(std::move(socket));
  conn.set_recv_timeout_ms(30000);
  if (!send_register_cfg(conn, cfg_)) {
    log_("ERROR", "host", "register send failed");
    return 2;
  }
  log_("INFO", "host", "register device " + cfg_.device_id);

  SessionGuard guard;
  uint64_t session_id = 0;
  std::vector<uint8_t> nonce;

  while (!stop_flag_.load()) {
    DecodedFrame frame;
    bool recv_timeout = false;
    if (!conn.recv(frame, &recv_timeout)) {
      if (recv_timeout) continue;
      break;
    }

    switch (frame.type) {
      case MsgType::RegisterAck: {
        json::Value payload = parse_payload_cfg(frame);
        if (!payload.get("ok") || !payload.get("ok")->as_bool()) {
          log_("ERROR", "host", "register rejected");
          return 2;
        }
        relay_ice_servers_ = parse_ice_servers(payload);
        guard.move_to(SessionState::Registered);
        log_("INFO", "host", "online, waiting for controller");
        if (status_cb_) status_cb_("registered");
        break;
      }
      case MsgType::SessionInvite: {
        session_id = frame.session_id;
        json::Value payload = parse_payload_cfg(frame);
        std::string from = "unknown";
        if (const json::Value* from_field = payload.get("from")) {
          if (const json::Value* id_field = from_field->get("id")) {
            from = id_field->as_string("unknown");
          }
        }
        // 主控在 ConnectReq.want 里声明可接受的最高帧率，这里记录后按需降帧编码。
        requested_fps_.store(0);  // 本次邀请未声明时不能沿用上一会话的值
        requested_transport_.clear();
        caps_negotiated_ = false;
        negotiated_codec_.clear();
        negotiated_quality_ = 0;
        negotiated_features_.clear();
        if (const json::Value* want_field = payload.get("want")) {
          if (const json::Value* fps_field = want_field->get("max_fps")) {
            requested_fps_.store(static_cast<int>(fps_field->as_int(0)));
          }
          // 主控声明期望的传输方式（"tcp" / "webrtc"），由被控端决定最终值。
          if (const json::Value* transport_field = want_field->get("transport")) {
            requested_transport_ = transport_field->as_string("tcp");
          }
        }
        guard.move_to(SessionState::Invited);
        json::Value accept = make_object();
        accept.set("session_id", static_cast<int64_t>(session_id));
        conn.send_json(MsgType::SessionAccept, session_id, accept);

        nonce = make_nonce();
        json::Value challenge = make_object();
        challenge.set("session_id", static_cast<int64_t>(session_id));
        challenge.set("nonce", base64_encode(nonce));
        challenge.set("method", "password");
        conn.send_json(MsgType::AuthChallenge, session_id, challenge);
        guard.move_to(SessionState::Authing);
        log_("INFO", "host", "session " + std::to_string(session_id) + " invited by " + from);
        if (status_cb_) status_cb_("authing");
        break;
      }
      case MsgType::CapsNegotiate: {
        if (frame.session_id != session_id) break;
        json::Value payload = parse_payload_cfg(frame);
        caps_negotiated_ = true;
        negotiated_codec_ = pick_codec(cfg_.codec, payload.get("codec"));
        // WebRTC 视频轨只打包 H.264；协商到其它编码时退回 TCP，避免建出无法解码的会话。
        if (requested_transport_ == "webrtc" && negotiated_codec_ != "h264") {
          log_("WARN", "webrtc",
               "caps negotiated codec=" + negotiated_codec_ + ", falling back to tcp");
          requested_transport_ = "tcp";
        }

        // 主控能力只用于收敛，不会把本端配置抬高；最高帧率取双方较小值。
        int fps = cfg_.fps > 0 ? cfg_.fps : 60;
        if (const json::Value* field = payload.get("max_fps")) {
          const int client_fps = static_cast<int>(field->as_int(0));
          if (client_fps > 0 && client_fps < fps) fps = client_fps;
        }
        requested_fps_.store(fps);

        int quality = cfg_.quality;
        if (const json::Value* field = payload.get("quality")) {
          const int client_quality = static_cast<int>(field->as_int(0));
          if (client_quality > 0 && client_quality < quality) quality = client_quality;
        }
        negotiated_quality_ = quality;

        negotiated_features_.clear();
        const json::Value* features = payload.get("features");
        if (features && features->is_array()) {
          for (std::size_t i = 0; i < features->size(); ++i) {
            const std::string name = features->at(i).as_string();
            if (feature_supported(name)) negotiated_features_.push_back(name);
          }
        }
        log_("INFO", "host",
             "caps negotiate: codec=" + negotiated_codec_ + " fps=" + std::to_string(fps) +
                 " quality=" + std::to_string(quality) + " features=" +
                 std::to_string(negotiated_features_.size()));
        break;
      }
      case MsgType::AuthResponse: {
        json::Value payload = parse_payload_cfg(frame);
        const std::string proof =
            payload.get("password_proof") ? payload.get("password_proof")->as_string() : "";
        const bool ok = verify_password_proof(cfg_.password, nonce, proof);

        json::Value result = make_object();
        result.set("ok", ok);
        // 告知主控端最终使用的传输方式：请求了 WebRTC 但本端不支持时回落 TCP。
        std::string transport = "tcp";
#ifdef RC_ENABLE_WEBRTC
        if (requested_transport_ == "webrtc") transport = "webrtc";
#else
        if (requested_transport_ == "webrtc") {
          log_("WARN", "webrtc",
               "controller requested webrtc but this build has no WebRTC support, using tcp");
        }
#endif
        if (!ok) {
          result.set("reason", "bad password");
        } else {
          result.set("transport", transport);
        }
        conn.send_json(MsgType::AuthResult, session_id, result);

        if (!ok) {
          log_("WARN", "host", "authentication failed");
          guard.move_to(SessionState::Registered);
          session_id = 0;
          nonce.clear();
          requested_transport_.clear();
          if (status_cb_) status_cb_("registered");
          break;
        }
        guard.move_to(SessionState::Ready);
        guard.move_to(SessionState::Streaming);
        log_("INFO", "host", "authentication passed, starting capture/encode (transport=" +
                              transport + ")");
        stream_frames_(conn, session_id, transport == "webrtc");
        guard = SessionGuard(SessionState::Registered);
        session_id = 0;
        nonce.clear();
        requested_transport_.clear();
        log_("INFO", "host", "session ended, waiting for next controller");
        if (status_cb_) status_cb_("registered");
        break;
      }
      case MsgType::SessionClose: {
        json::Value ack = make_object();
        ack.set("reason", "ack");
        conn.send_json(MsgType::SessionClose, session_id, ack);
        guard = SessionGuard(SessionState::Registered);
        session_id = 0;
        nonce.clear();
        log_("INFO", "host", "session closed, waiting for next controller");
        if (status_cb_) status_cb_("registered");
        break;
      }
      default:
        break;
    }
  }

  log_("INFO", "host", "exit state=" + std::string(state_name(guard.state())));
  return 0;
}

ClientRunner::ClientRunner(const SessionConfig& cfg) : cfg_(cfg) {}
ClientRunner::~ClientRunner() { stop(); }

void ClientRunner::stop() {
  stop_flag_.store(true);
  input_stop_.store(true);
  input_cv_.notify_all();
  file_cv_.notify_all();
}

void ClientRunner::queue_file(const std::string& path) {
  if (path.empty()) return;
  {
    std::lock_guard<std::mutex> lock(file_mutex_);
    file_queue_.push_back(path);
  }
  file_cv_.notify_one();
}

void ClientRunner::send_input(const InputEvent& event) {
  if (input_stop_.load() || input_session_.load() == 0) return;
  {
    std::lock_guard<std::mutex> lock(input_mutex_);
    if (event.type == MsgType::InputMouseMove && !input_queue_.empty() &&
        input_queue_.back().type == MsgType::InputMouseMove) {
      input_queue_.back() = event;
    } else {
      input_queue_.push_back(event);
    }
    // 输入只保留最新状态，避免网络变慢后继续执行一串过期鼠标事件。
    while (input_queue_.size() > 512) input_queue_.pop_front();
  }
  input_cv_.notify_one();
}

void ClientRunner::log_(const std::string& level, const std::string& tag, const std::string& msg) {
  if (log_cb_) {
    log_cb_(tag, level, msg);
  } else {
    if (level == "ERROR") RC_LOGE(tag, msg);
    else if (level == "WARN") RC_LOGW(tag, msg);
    else RC_LOGI(tag, msg);
  }
}

int ClientRunner::run() {
  if (cfg_.target_id.empty()) {
    log_("ERROR", "ctrl", "target device id is required");
    return 1;
  }

  std::string error;
  TcpSocket socket = TcpSocket::connect(cfg_.relay_host, cfg_.relay_port, &error,
                                        cfg_.connect_timeout_ms, cfg_.tcp_no_delay);
  if (!socket.valid()) {
    log_("ERROR", "ctrl", "relay connect failed: " + error);
    return 2;
  }
  if (cfg_.tls) {
    std::string tls_error;
    if (!socket.start_tls_client(cfg_.tls_server_name, cfg_.tls_ca,
                                 cfg_.tls_fingerprint, cfg_.tls_insecure,
                                 &tls_error)) {
      log_("ERROR", "ctrl", "relay TLS failed: " + tls_error);
      return 2;
    }
    log_("INFO", "ctrl", "relay TLS established" +
                             (socket.tls_peer_fingerprint().empty()
                                  ? std::string()
                                  : ", cert sha256=" + socket.tls_peer_fingerprint()));
  }
  log_("INFO", "ctrl", "connected to relay " + cfg_.relay_host + ":" + std::to_string(cfg_.relay_port));
  if (status_cb_) status_cb_("connected");

  Connection conn(std::move(socket));
  conn.set_recv_timeout_ms(15000);
  if (!send_register_cfg(conn, cfg_)) {
    log_("ERROR", "ctrl", "register send failed");
    return 2;
  }

  while (!stop_flag_.load()) {
    DecodedFrame frame;
    if (!conn.recv(frame)) {
      log_("ERROR", "ctrl", "no register ack from relay");
      return 2;
    }
    if (frame.type == MsgType::RegisterAck) {
      json::Value payload = parse_payload_cfg(frame);
      if (!payload.get("ok") || !payload.get("ok")->as_bool()) {
        log_("ERROR", "ctrl", "register rejected");
        return 2;
      }
      relay_ice_servers_ = parse_ice_servers(payload);
      break;
    }
  }
  log_("INFO", "ctrl", "registered, requesting session with " + cfg_.target_id);

  SessionGuard guard;
  uint64_t session_id = 0;
  std::vector<uint8_t> nonce;
  int frames_received = 0;
  uint64_t bytes_received = 0;

  // 关键帧请求：会话建立时主动要一帧 IDR；解码失败时带 500ms 节流再要一次，
  // 避免码流损坏导致每帧都发请求把控制通道打满。
  std::atomic<uint64_t> keyframe_session{0};
  std::atomic<int64_t> last_keyframe_req_ms{0};
  // 剪贴板消息必须等会话建立后再发，鉴权前 watcher 只记录不发送。
  std::atomic<uint64_t> clipboard_session{0};
  int pending_clipboard_size = -1;
  // 文件传输：会话建立后由独立线程按队列顺序发送，等待 FileAccept 有超时。
  std::atomic<uint64_t> file_session{0};
  std::mutex file_accept_mutex;
  std::condition_variable file_accept_cv;
  uint32_t pending_file_id = 0;
  bool file_accept_ready = false;
  bool file_accepted = false;
  std::string file_accept_reason;
  std::atomic<uint32_t> file_abort_id{0};
  std::atomic<uint32_t> next_transfer_id{1};
  // VideoStats 每秒到达，日志按 5s 取样，避免刷屏。
  std::chrono::steady_clock::time_point last_stats_log{};
  auto request_keyframe = [&](const char* reason) {
    const uint64_t sid = keyframe_session.load();
    if (sid == 0) return;
    const int64_t now = static_cast<int64_t>(now_ms());
    int64_t prev = last_keyframe_req_ms.load();
    if (prev != 0 && now - prev < 500) return;
    if (!last_keyframe_req_ms.compare_exchange_strong(prev, now)) return;
    json::Value body = make_object();
    body.set("reason", reason);
    if (conn.send_json(MsgType::KeyframeReq, sid, body)) {
      log_("INFO", "ctrl", std::string("requested keyframe (") + reason + ")");
    } else {
      log_("WARN", "ctrl", "send keyframe request failed");
    }
  };

  json::Value request = make_object();
  request.set("target_id", cfg_.target_id);
  request.set("mode", "control");
  json::Value want = make_object();
  want.set("codec", cfg_.codec);
  want.set("max_fps", cfg_.fps);
  want.set("transport", cfg_.transport);
  request.set("want", want);
  conn.send_json(MsgType::ConnectReq, 0, request);
  guard.move_to(SessionState::Connecting);
  log_("INFO", "ctrl", "requesting session with " + cfg_.target_id);

  // 第 4 点：解码放到独立线程，收包线程不再被解码耗时阻塞；
  // 队列深度 1，解码跟不上时覆盖旧帧而不是排队累积延迟。
  std::unique_ptr<IRenderer> renderer;
  struct RenderFrame {
    std::vector<uint8_t> bytes;
    int width = 0;
    int height = 0;
    int stride = 0;
    int coded_height = 0;
    bool nv12 = false;
  };
  std::mutex render_mutex;
  RenderFrame render_frame;
  bool render_has_frame = false;

  if (cfg_.render && !frame_cb_) {
#ifdef __linux__
    renderer = std::make_unique<SdlRenderer>();
#else
    renderer = std::make_unique<WindowRenderer>();
#endif
    std::string render_err;
    if (!renderer->open("rc ctrl", 1280, 720, &render_err)) {
      log_("WARN", "ctrl", "renderer open failed: " + render_err + ", fallback no-render");
      renderer.reset();
    } else {
      log_("INFO", "ctrl", "renderer=" + std::string(renderer->name()));
    }
  }

  std::mutex decode_mutex;
  std::condition_variable decode_cv;
  std::vector<uint8_t> decode_payload;
  VideoFrameMeta decode_meta;
  bool decode_has_frame = false;
  // WebRTC 帧已经解包成完整 Annex-B，不再带 32 字节视频头。
  bool decode_webrtc = false;
  std::atomic<bool> decode_stop{false};
  std::atomic<int> frames_decoded{0};
  std::atomic<uint64_t> total_decode_us{0};

  std::thread decode_thread([&]() {
    std::unique_ptr<IVideoDecoder> decoder;
    DecodedImage picture;
    while (true) {
      std::vector<uint8_t> payload;
      VideoFrameMeta meta;
      bool webrtc_frame = false;
      {
        std::unique_lock<std::mutex> lock(decode_mutex);
        decode_cv.wait(lock, [&] { return decode_has_frame || decode_stop.load(); });
        if (decode_stop.load() && !decode_has_frame) break;
        payload = std::move(decode_payload);
        meta = decode_meta;
        webrtc_frame = decode_webrtc;
        decode_webrtc = false;
        decode_has_frame = false;
      }

      const std::size_t data_len = webrtc_frame
          ? payload.size()
          : (payload.size() >= kVideoHeaderBytes ? payload.size() - kVideoHeaderBytes : 0);
      if (data_len == 0) continue;
      const uint8_t* data =
          webrtc_frame ? payload.data() : payload.data() + kVideoHeaderBytes;

      if (!decoder) {
        std::string dec_error;
        decoder = create_decoder(&dec_error);
        if (!decoder) {
          log_("WARN", "ctrl", "decoder create failed: " + dec_error);
          continue;
        }
        // GPU 上屏的渲染器直接吃 NV12，省掉解码后的 NV12->BGRA 转换（实测约 14ms/帧）。
        const bool nv12_direct = renderer && renderer->gpu() && !frame_cb_;
        decoder->set_output(nv12_direct ? IVideoDecoder::Output::kNv12
                                        : IVideoDecoder::Output::kBgra);
        if (!decoder->open(&dec_error)) {
          log_("WARN", "ctrl", "decoder open failed: " + dec_error);
          decoder.reset();
        }
      }
      if (!decoder) continue;

      std::string dec_error;
      const uint64_t t0 = now_us();
      if (decoder->decode(data, data_len, meta.capture_ts_us, picture, &dec_error)) {
        total_decode_us.fetch_add(now_us() - t0);
        frames_decoded.fetch_add(1);
        // 交给上层显示（GUI 在这里做深拷贝再跨线程投递）。
        if (frame_cb_ && !picture.pixels.empty() && !picture.nv12 && picture.stride > 0) {
          // GUI 需要 BGRA 裸数据，自行深拷贝后跨线程投递。
          frame_cb_(picture.pixels.data(), picture.width, picture.height, picture.stride);
        } else if (renderer && !picture.pixels.empty() && picture.stride > 0) {
          RenderFrame rf;
          rf.bytes = picture.pixels;
          rf.width = picture.width;
          rf.height = picture.height;
          rf.stride = picture.stride;
          rf.coded_height = picture.coded_height;
          rf.nv12 = picture.nv12;
          {
            std::lock_guard<std::mutex> lk(render_mutex);
            // 渲染帧交给主线程上屏
            render_frame = std::move(rf);
            render_has_frame = true;
          }
        }
      } else {
        // 解码失败通常意味着丢了参考帧：立刻向被控端要一帧 IDR 重新同步。
        request_keyframe("decode-failed");
      }
    }
  });

  // 第 1 点残余：主控端空闲时定期发心跳，避免被控端 30s 收不到输入事件而结束会话。
  std::atomic<bool> hb_stop{false};
  std::atomic<uint64_t> hb_session{0};
  std::thread hb_thread([&]() {
    while (!hb_stop.load()) {
      for (int i = 0; i < 20 && !hb_stop.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
      }
      if (hb_stop.load()) break;
      const uint64_t sid = hb_session.load();
      if (sid == 0) continue;
      if (!conn.send(MsgType::Heartbeat, sid, nullptr, 0)) break;
    }
  });

  // 任何 return 路径都必须先停线程再 join，否则 std::thread 析构会 terminate。
  struct DecodeJoiner {
    std::atomic<bool>* stop;
    std::condition_variable* cv;
    std::thread* thread;
    ~DecodeJoiner() {
      stop->store(true);
      cv->notify_all();
      if (thread->joinable()) thread->join();
    }
  } decode_joiner{&decode_stop, &decode_cv, &decode_thread};

  struct HeartbeatJoiner {
    std::atomic<bool>* stop;
    std::thread* thread;
    ~HeartbeatJoiner() {
      stop->store(true);
      if (thread->joinable()) thread->join();
    }
  } hb_joiner{&hb_stop, &hb_thread};

#ifdef RC_ENABLE_WEBRTC
  // 声明在解码线程之后：析构先于解码状态，避免回调访问已释放的队列。
  std::unique_ptr<WebRtcClient> rtc_client;
  std::atomic<WebRtcClient*> rtc_active{nullptr};
#endif

  std::thread input_thread([&]() {
    while (true) {
      InputEvent event;
      {
        std::unique_lock<std::mutex> lock(input_mutex_);
        input_cv_.wait(lock, [&] { return input_stop_.load() || !input_queue_.empty(); });
        if (input_stop_.load()) break;
        event = input_queue_.front();
        input_queue_.pop_front();
      }
      const uint64_t sid = input_session_.load();
      if (sid == 0) continue;
      const std::vector<uint8_t> payload = encode_input_event(event);
      if (payload.empty()) continue;
#ifdef RC_ENABLE_WEBRTC
      if (WebRtcClient* rtc = rtc_active.load()) {
        // DataChannel 消息前置 1 字节 MsgType，被控端据此复用同一套注入逻辑。
        std::vector<uint8_t> message;
        message.reserve(payload.size() + 1);
        message.push_back(static_cast<uint8_t>(event.type));
        message.insert(message.end(), payload.begin(), payload.end());
        rtc->send_input(message.data(), message.size());
        continue;
      }
#endif
      if (!conn.send(event.type, sid, payload.data(), payload.size())) {
        stop_flag_.store(true);
        break;
      }
    }
  });
  struct InputJoiner {
    std::atomic<bool>* stop;
    std::condition_variable* cv;
    std::thread* thread;
    ~InputJoiner() {
      stop->store(true);
      cv->notify_all();
      if (thread->joinable()) thread->join();
    }
  } input_joiner{&input_stop_, &input_cv_, &input_thread};

  // 文件发送：一次一个文件，FileOffer → 等 FileAccept（10s）→ 64 KiB 分块 → FileEnd。
  std::atomic<bool> file_stop{false};
  std::thread file_thread([&]() {
    while (!file_stop.load() && !stop_flag_.load()) {
      std::string path;
      {
        std::unique_lock<std::mutex> lock(file_mutex_);
        file_cv_.wait_for(lock, std::chrono::milliseconds(200), [&] {
          return file_stop.load() || stop_flag_.load() || !file_queue_.empty();
        });
        if (file_stop.load() || stop_flag_.load()) break;
        if (file_queue_.empty()) continue;
        path = file_queue_.front();
        file_queue_.pop_front();
      }

      FileSender sender;
      std::string error;
      if (!sender.open(path, &error)) {
        log_("ERROR", "file", "cannot open " + path + ": " + error);
        continue;
      }
      const uint64_t sid = file_session.load();
      if (sid == 0) {
        log_("WARN", "file", "session not ready, skip sending " + path);
        continue;
      }
      const uint32_t transfer_id = next_transfer_id.fetch_add(1);
      json::Value offer = make_object();
      offer.set("transfer_id", static_cast<int64_t>(transfer_id));
      offer.set("name", sender.name());
      offer.set("size", static_cast<int64_t>(sender.size()));
      {
        std::lock_guard<std::mutex> lock(file_accept_mutex);
        pending_file_id = transfer_id;
        file_accept_ready = false;
        file_accepted = false;
        file_accept_reason.clear();
      }
      if (!conn.send_json(MsgType::FileOffer, sid, offer)) {
        log_("WARN", "file", "send file offer failed");
        continue;
      }
      log_("INFO", "file", "offering " + sender.name() + " (" +
                                std::to_string(sender.size()) + " bytes)");
      bool accepted = false;
      std::string reason;
      {
        std::unique_lock<std::mutex> lock(file_accept_mutex);
        const bool got = file_accept_cv.wait_for(lock, std::chrono::seconds(10), [&] {
          return file_accept_ready || file_stop.load() || stop_flag_.load();
        });
        if (got && file_accept_ready) {
          accepted = file_accepted;
          reason = file_accept_reason;
        } else if (!got) {
          reason = "timeout";
        }
      }
      if (!accepted) {
        log_("WARN", "file", "transfer rejected: " + (reason.empty() ? "aborted" : reason));
        continue;
      }

      bool ok = true;
      uint64_t offset = 0;
      std::vector<uint8_t> chunk;
      while (!file_stop.load() && !stop_flag_.load()) {
        if (!sender.read_chunk(chunk, &error)) {
          ok = false;
          break;
        }
        if (chunk.empty()) break;
        if (file_abort_id.load() == transfer_id) {
          error = "aborted by host";
          ok = false;
          break;
        }
        std::vector<uint8_t> payload(kFileChunkHeaderBytes + chunk.size());
        write_u32_le(payload.data(), transfer_id);
        write_u64_le(payload.data() + 4, offset);
        std::memcpy(payload.data() + kFileChunkHeaderBytes, chunk.data(), chunk.size());
        if (!conn.send(MsgType::FileChunk, sid, payload.data(), payload.size())) {
          error = "send failed";
          ok = false;
          break;
        }
        offset += chunk.size();
      }
      if (ok) {
        json::Value end = make_object();
        end.set("transfer_id", static_cast<int64_t>(transfer_id));
        if (!conn.send_json(MsgType::FileEnd, sid, end)) {
          error = "send end failed";
          ok = false;
        }
      }
      if (ok) {
        log_("INFO", "file", "file sent: " + sender.name() + " (" +
                                  std::to_string(offset) + " bytes)");
      } else if (!file_stop.load() && !stop_flag_.load()) {
        json::Value abort_msg = make_object();
        abort_msg.set("transfer_id", static_cast<int64_t>(transfer_id));
        abort_msg.set("reason", error);
        conn.send_json(MsgType::FileAbort, sid, abort_msg);
        log_("WARN", "file", "transfer failed: " + error);
      }
    }
  });
  struct FileJoiner {
    std::atomic<bool>* stop;
    std::condition_variable* cv;
    std::thread* thread;
    ~FileJoiner() {
      stop->store(true);
      cv->notify_all();
      if (thread->joinable()) thread->join();
    }
  } file_joiner{&file_stop, &file_cv_, &file_thread};

  // 剪贴板同步：与会话同生命周期，析构时自动停止 watcher。
  std::unique_ptr<ClipboardBridge> clipboard;
  if (cfg_.clipboard) {
    clipboard = std::make_unique<ClipboardBridge>(
        [&](const std::string& text) {
          const uint64_t sid = clipboard_session.load();
          if (sid == 0) return true;  // 会话尚未建立，等下一次轮询
          json::Value announce = make_object();
          announce.set("format", "text");
          announce.set("size", static_cast<int64_t>(text.size()));
          if (!conn.send_json(MsgType::ClipboardSet, sid, announce)) return false;
          return conn.send(MsgType::ClipboardData, sid,
                           reinterpret_cast<const uint8_t*>(text.data()), text.size());
        },
        [&](const std::string& level, const std::string& msg) { log_(level, "clipboard", msg); });
    clipboard->start();
  }

  while (!stop_flag_.load()) {
    if (renderer) {
      renderer->pump();
      if (!renderer->alive()) {
        log_("INFO", "ctrl", "renderer window closed by user");
        break;
      }
      RenderFrame rf;
      bool has_frame = false;
      {
        std::lock_guard<std::mutex> lk(render_mutex);
        if (render_has_frame) {
          rf = std::move(render_frame);
          render_has_frame = false;
          has_frame = true;
        }
      }
      if (has_frame) {
        if (rf.nv12) {
          if (!renderer->present_nv12(rf.bytes.data(), rf.width, rf.height,
                                      rf.coded_height > 0 ? rf.coded_height : rf.height,
                                      rf.stride)) {
            log_("WARN", "ctrl", "renderer rejected nv12 frame");
          }
        } else {
          renderer->present(rf.bytes.data(), rf.width, rf.height, rf.stride);
        }
      }
    }

    DecodedFrame frame;
    if (!conn.recv(frame)) {
      // 收包失败通常是 15s 无响应（被控端不回应会话请求）或对端直接断开，
      // 之前这里静默 break，用户只看到黑窗然后程序消失，无法判断原因。
      log_("ERROR", "ctrl", "no data from relay within 15s while waiting for host, state=" +
                        std::string(state_name(guard.state())));
      return 2;
    }

    switch (frame.type) {
      case MsgType::ConnectRsp: {
        json::Value payload = parse_payload_cfg(frame);
        const json::Value* ok_field = payload.get("ok");
        if (!ok_field || !ok_field->as_bool()) {
          const json::Value* reason = payload.get("reason");
          std::string r = reason ? reason->as_string() : "unknown";
          log_("WARN", "ctrl", "connect rejected: " + r);
          return 2;
        }
        session_id = frame.session_id;
        hb_session.store(session_id);
        input_session_.store(session_id);
        clipboard_session.store(session_id);
        guard.move_to(SessionState::Ready);

        // 鉴权前先声明本端解码能力，让被控端在开编码器前收敛参数。
        json::Value caps = make_object();
        json::Value codecs = json::Value::make_array();
        codecs.push("h264");  // 当前主控端只实现 H.264 解码
        caps.set("codec", codecs);
        caps.set("max_fps", cfg_.fps);
        caps.set("quality", cfg_.quality);
        json::Value features = json::Value::make_array();
        features.push("keyframe-request");
        features.push("video-stats");
        caps.set("features", features);
        if (!conn.send_json(MsgType::CapsNegotiate, session_id, caps)) {
          log_("WARN", "ctrl", "send caps negotiate failed");
        }
        log_("INFO", "ctrl", "session " + std::to_string(session_id) + " accepted by host");
        break;
      }
      case MsgType::AuthChallenge: {
        json::Value payload = parse_payload_cfg(frame);
        nonce.clear();
        const json::Value* nonce_field = payload.get("nonce");
        if (nonce_field) nonce = base64_decode(nonce_field->as_string());

        std::string proof = password_proof(cfg_.password, nonce);
        json::Value resp = make_object();
        resp.set("password_proof", proof);
        conn.send_json(MsgType::AuthResponse, session_id, resp);
        guard.move_to(SessionState::Authing);
        if (status_cb_) status_cb_("authing");
        break;
      }
      case MsgType::AuthResult: {
        json::Value payload = parse_payload_cfg(frame);
        bool ok = payload.get("ok") && payload.get("ok")->as_bool();
        if (!ok) {
          log_("WARN", "ctrl", "authentication failed");
          if (status_cb_) status_cb_("auth_failed");
          return 3;
        }
#ifdef RC_ENABLE_WEBRTC
        // 被控端在 AuthResult 中告知最终传输方式；请求 WebRTC 且两端都支持时才切换。
        const std::string transport =
            payload.get("transport") ? payload.get("transport")->as_string("tcp") : "tcp";
        if (transport == "webrtc") {
          rtc_client = std::make_unique<WebRtcClient>();
          WebRtcClient::Callbacks rtc_cb;
          rtc_cb.signaling.on_local_description =
              [&conn, session_id](const std::string& type, const std::string& sdp) {
                json::Value body = make_object();
                body.set("type", type);
                body.set("sdp", sdp);
                conn.send_json(MsgType::WebRtcAnswer, session_id, body);
              };
          rtc_cb.signaling.on_local_candidate =
              [&conn, session_id](const std::string& candidate, const std::string& mid) {
                json::Value body = make_object();
                body.set("candidate", candidate);
                body.set("mid", mid);
                conn.send_json(MsgType::WebRtcCandidate, session_id, body);
              };
          rtc_cb.on_video = [&](const uint8_t* data, std::size_t size,
                                uint32_t /*rtp_timestamp*/) {
            // 网络线程：只入队，解码交给独立线程。
            {
              std::lock_guard<std::mutex> lock(decode_mutex);
              decode_payload.assign(data, data + size);
              decode_meta = VideoFrameMeta{};
              decode_webrtc = true;
              decode_has_frame = true;
            }
            decode_cv.notify_one();
            ++frames_received;
            bytes_received += size;
            if (progress_cb_) {
              progress_cb_(frames_received, static_cast<int64_t>(bytes_received));
            }
            // VideoFrame 走 TCP 时的帧数上限判断在 recv 循环里，这里补上 WebRTC 路径。
            if (cfg_.max_frames > 0 && frames_received >= cfg_.max_frames) {
              stop_flag_.store(true);
            }
          };
          std::string rtc_error;
          WebRtcConfig rtc_config;
          rtc_config.ice_servers =
              cfg_.ice_servers.empty() ? relay_ice_servers_ : cfg_.ice_servers;
          rtc_config.force_relay = cfg_.ice_force_relay;
          rtc_config.enable_ice_tcp = cfg_.ice_tcp;
          if (!rtc_config.ice_servers.empty()) {
            log_("INFO", "webrtc", "ice servers=" +
                                     std::to_string(rtc_config.ice_servers.size()));
          }
          if (!rtc_client->start(rtc_cb, rtc_config, &rtc_error)) {
            log_("ERROR", "webrtc", "client start failed: " + rtc_error);
            rtc_client.reset();
          } else {
            rtc_active.store(rtc_client.get());
            log_("INFO", "webrtc", "transport enabled, waiting for offer");
          }
        } else if (cfg_.transport == "webrtc") {
          log_("WARN", "webrtc", "host does not support webrtc, falling back to tcp");
        }
#endif
        guard.move_to(SessionState::Ready);
        guard.move_to(SessionState::Streaming);
        log_("INFO", "ctrl", "authenticated");
        keyframe_session.store(session_id);
        file_session.store(session_id);
        request_keyframe("start");
        if (status_cb_) status_cb_("streaming");
        break;
      }
      case MsgType::WebRtcOffer: {
#ifdef RC_ENABLE_WEBRTC
        if (rtc_client) {
          json::Value payload = parse_payload_cfg(frame);
          const json::Value* sdp = payload.get("sdp");
          std::string rtc_error;
          if (!sdp || !rtc_client->set_offer(sdp->as_string(), &rtc_error)) {
            log_("ERROR", "webrtc", "apply offer failed: " + rtc_error);
          } else {
            log_("INFO", "webrtc", "offer applied, answer sent");
          }
        }
#endif
        break;
      }
      case MsgType::WebRtcCandidate: {
#ifdef RC_ENABLE_WEBRTC
        if (rtc_client) {
          json::Value payload = parse_payload_cfg(frame);
          const json::Value* candidate = payload.get("candidate");
          const json::Value* mid = payload.get("mid");
          if (candidate) {
            rtc_client->add_remote_candidate(candidate->as_string(),
                                             mid ? mid->as_string() : "video");
          }
        }
#endif
        break;
      }
      case MsgType::ClipboardSet: {
        json::Value payload = parse_payload_cfg(frame);
        pending_clipboard_size =
            payload.get("size") ? static_cast<int>(payload.get("size")->as_int(-1)) : -1;
        break;
      }
      case MsgType::ClipboardData: {
        if (clipboard && pending_clipboard_size >= 0 &&
            static_cast<int>(frame.payload.size()) == pending_clipboard_size) {
          clipboard->apply_remote(std::string(frame.payload.begin(), frame.payload.end()));
        }
        pending_clipboard_size = -1;
        break;
      }
      case MsgType::FileAccept: {
        json::Value payload = parse_payload_cfg(frame);
        const uint32_t tid =
            payload.get("transfer_id")
                ? static_cast<uint32_t>(payload.get("transfer_id")->as_int(0))
                : 0;
        const bool accept = payload.get("accept") && payload.get("accept")->as_bool();
        const std::string reason =
            payload.get("reason") ? payload.get("reason")->as_string("") : "";
        {
          std::lock_guard<std::mutex> lock(file_accept_mutex);
          if (tid == pending_file_id) {
            file_accept_ready = true;
            file_accepted = accept;
            file_accept_reason = reason;
          }
        }
        file_accept_cv.notify_all();
        break;
      }
      case MsgType::FileAbort: {
        json::Value payload = parse_payload_cfg(frame);
        const uint32_t tid =
            payload.get("transfer_id")
                ? static_cast<uint32_t>(payload.get("transfer_id")->as_int(0))
                : 0;
        const std::string reason =
            payload.get("reason") ? payload.get("reason")->as_string("") : "";
        file_abort_id.store(tid);
        log_("WARN", "file",
             "transfer aborted by host" + (reason.empty() ? "" : ": " + reason));
        break;
      }
      case MsgType::CapsNegotiate: {
        // 被控端回执：实际采用的编码/分辨率/帧率/画质，以及双方都支持的特性。
        json::Value payload = parse_payload_cfg(frame);
        const std::string codec =
            payload.get("codec") ? payload.get("codec")->as_string(cfg_.codec) : cfg_.codec;
        const int caps_width =
            payload.get("width") ? static_cast<int>(payload.get("width")->as_int(0)) : 0;
        const int caps_height =
            payload.get("height") ? static_cast<int>(payload.get("height")->as_int(0)) : 0;
        const int caps_fps =
            payload.get("fps") ? static_cast<int>(payload.get("fps")->as_int(0)) : 0;
        const int caps_quality =
            payload.get("quality") ? static_cast<int>(payload.get("quality")->as_int(0)) : 0;
        std::string features;
        const json::Value* list = payload.get("features");
        if (list && list->is_array()) {
          for (std::size_t i = 0; i < list->size(); ++i) {
            if (!features.empty()) features += ",";
            features += list->at(i).as_string();
          }
        }
        log_("INFO", "ctrl",
             "caps negotiated: codec=" + codec + " " + std::to_string(caps_width) + "x" +
                 std::to_string(caps_height) + " fps=" + std::to_string(caps_fps) +
                 " quality=" + std::to_string(caps_quality) +
                 (features.empty() ? "" : " features=" + features));
        break;
      }
      case MsgType::VideoStats: {
        const auto now_tp = std::chrono::steady_clock::now();
        if (last_stats_log.time_since_epoch().count() == 0 ||
            now_tp - last_stats_log >= std::chrono::seconds(5)) {
          last_stats_log = now_tp;
          json::Value payload = parse_payload_cfg(frame);
          auto stat_int = [&payload](const char* key) -> int64_t {
            const json::Value* field = payload.get(key);
            return field ? field->as_int(0) : 0;
          };
          log_("INFO", "ctrl",
               "stats: sent=" + std::to_string(stat_int("frames_sent")) + " bytes=" +
                   std::to_string(stat_int("bytes_sent")) + " fps=" +
                   std::to_string(stat_int("fps")) + " bitrate=" +
                   std::to_string(stat_int("bitrate_kbps")) + "kbps encode=" +
                   std::to_string(stat_int("encode_ms")) + "ms captured=" +
                   std::to_string(stat_int("captured")) + " dropped=" +
                   std::to_string(stat_int("dropped")));
        }
        break;
      }
      case MsgType::VideoConfig: {
        json::Value payload = parse_payload_cfg(frame);
        const std::string codec =
            payload.get("codec") ? payload.get("codec")->as_string(cfg_.codec) : cfg_.codec;
        const int cfg_width =
            payload.get("width") ? static_cast<int>(payload.get("width")->as_int(0)) : 0;
        const int cfg_height =
            payload.get("height") ? static_cast<int>(payload.get("height")->as_int(0)) : 0;
        const int cfg_fps =
            payload.get("fps") ? static_cast<int>(payload.get("fps")->as_int(0)) : 0;
        const int cfg_quality =
            payload.get("quality") ? static_cast<int>(payload.get("quality")->as_int(0)) : 0;
        const int cfg_kbps = payload.get("bitrate_kbps")
                                 ? static_cast<int>(payload.get("bitrate_kbps")->as_int(0))
                                 : 0;
        log_("INFO", "ctrl",
             "video config: codec=" + codec + " " + std::to_string(cfg_width) + "x" +
                 std::to_string(cfg_height) + " fps=" + std::to_string(cfg_fps) +
                 " quality=" + std::to_string(cfg_quality) +
                 (cfg_kbps > 0 ? " bitrate=" + std::to_string(cfg_kbps) + "kbps" : ""));
        break;
      }
      case MsgType::VideoFrame: {
        VideoFrameMeta meta;
        if (!parse_video_payload(frame.payload, meta)) break;

        const std::size_t payload_size = frame.payload.size();
        {
          std::lock_guard<std::mutex> lock(decode_mutex);
          decode_payload = std::move(frame.payload);  // 深度 1：解码慢时覆盖旧帧
          decode_meta = meta;
          decode_has_frame = true;
        }
        decode_cv.notify_one();

        ++frames_received;
        bytes_received += payload_size;
        if (progress_cb_) {
          progress_cb_(frames_received, static_cast<int64_t>(bytes_received));
        }

        if (cfg_.max_frames > 0 && frames_received >= cfg_.max_frames) {
          guard.move_to(SessionState::Closing);
          return 0;
        }
        break;
      }
      case MsgType::SessionClose: {
        json::Value ack = make_object();
        ack.set("reason", "ack");
        conn.send_json(MsgType::SessionClose, session_id, ack);
        guard.move_to(SessionState::Closed);
        input_session_.store(0);
        clipboard_session.store(0);
        file_session.store(0);
        log_("INFO", "ctrl", "session closed by host");
        if (status_cb_) status_cb_("closed");
        return 0;
      }
      default:
        break;
    }
  }

  const int decoded = frames_decoded.load();
  if (decoded > 0) {
    log_("INFO", "ctrl", "frames_decoded=" + std::to_string(decoded) +
                           " avg_decode=" +
                           std::to_string(total_decode_us.load() / static_cast<uint64_t>(decoded)) +
                           "us");
  }
  return 0;
}

}  // namespace rc
