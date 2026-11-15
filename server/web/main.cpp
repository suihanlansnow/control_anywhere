// rc-web：把中继上的被控端画面转成 JPEG 供浏览器预览，并把浏览器的键鼠事件
// 转成现有输入协议转发给被控端。视频链路完全复用 ClientRunner（会话、鉴权、
// 解码），这里只负责 JPEG 转码和 HTTP/Web 侧适配。

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "base/clock.h"
#include "base/json.h"
#include "base/log.h"
#include "capture/capture.h"
#include "codec/encoder.h"
#include "net/socket.h"
#include "protocol/protocol.h"
#include "session/runner.h"
#include "session/session_config.h"

#include "http_server.h"
#include "web_page.h"

using namespace rc;
using namespace rc::web;

namespace {

std::atomic<bool> g_interrupted{false};

void on_signal(int) { g_interrupted.store(true); }

uint64_t steady_ms() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

struct Options {
  uint16_t http_port = 8080;
  std::string relay_host = "127.0.0.1";
  uint16_t relay_port = kDefaultControlPort;
  std::string target_id;
  std::string device_id = "web-client";
  std::string alias = "web";
  std::string password;
  std::string codec = "h264";
  int fps = 15;
  int quality = 70;
};

void print_usage() {
  std::printf(
      "用法: rc-web [--target <被控端设备号>] [--password <密码>]\n"
      "              [--http-port 8080] [--relay 127.0.0.1:21116]\n"
      "              [--device-id web-client] [--fps 15] [--quality 70]\n"
      "              [--codec h264]\n"
      "\n"
      "--target/--password 可省略：省略时启动后仅提供网页，由浏览器填写\n"
      "被控端设备 ID 与密码后再建立会话。\n"
      "\n"
      "示例: rc-web --http-port 8080\n"
      "      浏览器打开 http://<本机IP>:8080/ 并填入被控端信息\n");
}

bool split_host_port(const std::string& text, std::string& host, uint16_t& port) {
  const std::size_t colon = text.rfind(':');
  if (colon == std::string::npos) {
    host = text;
    return true;
  }
  host = text.substr(0, colon);
  port = static_cast<uint16_t>(std::atoi(text.c_str() + colon + 1));
  return true;
}

bool parse_options(int argc, char** argv, Options& opts) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto take_value = [&](std::string& out) {
      if (i + 1 >= argc) return false;
      out = argv[++i];
      return true;
    };
    std::string value;
    if (arg == "--target") {
      if (!take_value(opts.target_id)) return false;
    } else if (arg == "--password") {
      if (!take_value(opts.password)) return false;
    } else if (arg == "--http-port") {
      if (!take_value(value)) return false;
      opts.http_port = static_cast<uint16_t>(std::atoi(value.c_str()));
    } else if (arg == "--relay") {
      if (!take_value(value)) return false;
      split_host_port(value, opts.relay_host, opts.relay_port);
    } else if (arg == "--device-id") {
      if (!take_value(opts.device_id)) return false;
    } else if (arg == "--fps") {
      if (!take_value(value)) return false;
      opts.fps = std::atoi(value.c_str());
    } else if (arg == "--quality") {
      if (!take_value(value)) return false;
      opts.quality = std::atoi(value.c_str());
    } else if (arg == "--codec") {
      if (!take_value(opts.codec)) return false;
    } else if (arg == "--help" || arg == "-h") {
      return false;
    } else {
      return false;
    }
  }
  return true;
}

// 解码线程 -> 编码线程的原始 BGRA 帧槽（深度 1，覆盖旧帧）。
class RawFrameSlot {
 public:
  void push(const uint8_t* bgra, int width, int height, int stride) {
    const std::size_t row = static_cast<std::size_t>(width) * 4;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stopped_) return;
      data_.resize(row * static_cast<std::size_t>(height));
      if (stride == static_cast<int>(row)) {
        std::memcpy(data_.data(), bgra, row * static_cast<std::size_t>(height));
      } else {
        for (int y = 0; y < height; ++y) {
          std::memcpy(data_.data() + row * static_cast<std::size_t>(y),
                      bgra + static_cast<std::size_t>(y) *
                                 static_cast<std::size_t>(stride),
                      row);
        }
      }
      width_ = width;
      height_ = height;
      ++seq_;
    }
    cv_.notify_one();
  }

  bool wait_next(uint64_t& last_seq, std::vector<uint8_t>& out, int& width,
                 int& height, int timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                 [&] { return seq_ != last_seq || stopped_; });
    if (seq_ == last_seq || data_.empty()) return false;
    // 必须记录已消费的序号，否则同一帧会被反复取出重编码。
    last_seq = seq_;
    out.swap(data_);
    width = width_;
    height = height_;
    return true;
  }

  void stop() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopped_ = true;
    }
    cv_.notify_all();
  }

  bool stopped() {
    std::lock_guard<std::mutex> lock(mutex_);
    return stopped_;
  }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  std::vector<uint8_t> data_;
  int width_ = 0;
  int height_ = 0;
  uint64_t seq_ = 0;
  bool stopped_ = false;
};

struct Snapshot {
  std::shared_ptr<const std::vector<uint8_t>> jpeg;
  int width = 0;
  int height = 0;
  uint64_t seq = 0;
};

// 最新 JPEG 帧槽：多个浏览器同时观看时共享同一份数据，新帧到达唤醒所有等待者。
class FrameStore {
 public:
  void publish(std::shared_ptr<const std::vector<uint8_t>> jpeg, int width,
               int height) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      jpeg_ = std::move(jpeg);
      width_ = width;
      height_ = height;
      ++seq_;
      last_ms_ = steady_ms();
    }
    cv_.notify_all();
  }

  bool latest(Snapshot& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!jpeg_) return false;
    out.jpeg = jpeg_;
    out.width = width_;
    out.height = height_;
    out.seq = seq_;
    return true;
  }

  bool wait_next(uint64_t last_seq, Snapshot& out, int timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                 [&] { return seq_ != last_seq; });
    if (seq_ == last_seq || !jpeg_) return false;
    out.jpeg = jpeg_;
    out.width = width_;
    out.height = height_;
    out.seq = seq_;
    return true;
  }

  int64_t age_ms() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!jpeg_) return -1;
    return static_cast<int64_t>(steady_ms() - last_ms_);
  }

  // 切换目标或断开时清掉旧画面，避免新会话建立前短暂显示上一台设备。
  void clear() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      jpeg_.reset();
      width_ = 0;
      height_ = 0;
      ++seq_;
      last_ms_ = 0;
    }
    cv_.notify_all();
  }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  std::shared_ptr<const std::vector<uint8_t>> jpeg_;
  int width_ = 0;
  int height_ = 0;
  uint64_t seq_ = 0;
  uint64_t last_ms_ = 0;
};

class Gateway {
 public:
  explicit Gateway(const Options& options) : options_(options) {
    last_activity_ms_.store(steady_ms());
  }

  static constexpr int64_t kIdlePauseMs = 15000;  // 无活动多久后暂停会话

  // 浏览器提交的会话参数。启动时可为空，由 /api/connect 动态设置。
  struct SessionParams {
    std::string relay_host;
    uint16_t relay_port = kDefaultControlPort;
    std::string target_id;
    std::string password;
  };

  // 由网页提交被控端信息后建立会话；relay 为空时使用命令行默认中继。
  bool connect_session(const std::string& relay, const std::string& target,
                       const std::string& password, std::string& error) {
    std::string host = options_.relay_host;
    uint16_t port = options_.relay_port;
    if (!relay.empty()) {
      split_host_port(relay, host, port);
      if (host.empty() || port == 0) {
        error = "中继地址格式无效，应为 host:port";
        return false;
      }
    }
    if (target.empty()) {
      error = "请输入被控端设备 ID";
      return false;
    }
    if (password.empty()) {
      error = "请输入被控端密码";
      return false;
    }

    {
      std::lock_guard<std::mutex> lock(session_mutex_);
      session_ = SessionParams{host, port, target, password};
      configured_ = true;
      ++session_epoch_;
    }
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      state_ = "connecting";
    }
    {
      std::lock_guard<std::mutex> lock(runner_mutex_);
      if (runner_) runner_->stop();  // 停止上一台设备的会话
    }
    frames_.clear();
    note_activity();
    session_cv_.notify_all();
    RC_LOGI("web", "网页请求连接被控端 " << target << "，中继 " << host << ":" << port);
    return true;
  }

  void disconnect_session() {
    {
      std::lock_guard<std::mutex> lock(session_mutex_);
      configured_ = false;
      ++session_epoch_;
      session_ = SessionParams{};
    }
    {
      std::lock_guard<std::mutex> lock(runner_mutex_);
      if (runner_) runner_->stop();
    }
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      state_ = "idle";
    }
    frames_.clear();
    note_activity();
    session_cv_.notify_all();
    RC_LOGI("web", "网页断开当前会话");
  }

  bool configured() {
    std::lock_guard<std::mutex> lock(session_mutex_);
    return configured_;
  }

  FrameStore& frames() { return frames_; }
  bool stopping() const { return stopping_.load(); }

  void add_viewer(int delta) { viewers_.fetch_add(delta); }

  // 任何 HTTP 请求（页面、状态、推流、输入）都视为"有人在使用"，
  // 用于无人使用时暂停视频会话，避免解码链路空转。
  void note_activity() {
    last_activity_ms_.store(steady_ms());
    pause_cv_.notify_all();
  }

  // 编码线程独占 WIC/ffmpeg 编码器及其 COM apartment。ClientRunner 每次重连都会
  // 新建解码线程并在结束时 CoUninitialize，如果把编码器放在解码线程里复用，
  // 下一次会话就会在已销毁的 apartment 上调用 WIC 而崩溃。
  void start() {
    encoder_thread_ = std::thread([this] { encoder_loop(); });
  }

  void stop() {
    stopping_.store(true);
    raw_.stop();
    {
      std::lock_guard<std::mutex> lock(runner_mutex_);
      if (runner_) runner_->stop();
    }
    if (encoder_thread_.joinable()) encoder_thread_.join();
  }

  void run() {
    while (!stopping_.load()) {
      SessionParams params;
      uint64_t epoch = 0;
      {
        std::unique_lock<std::mutex> lock(session_mutex_);
        if (!configured_) {
          std::lock_guard<std::mutex> slock(state_mutex_);
          state_ = "idle";
        }
        // 未配置目标时等待网页提交被控端信息，避免无意义地连中继。
        session_cv_.wait(lock, [this] { return configured_ || stopping_.load(); });
        if (stopping_.load()) break;
        params = session_;
        epoch = session_epoch_;
      }

      SessionConfig cfg;
      cfg.relay_host = params.relay_host;
      cfg.relay_port = params.relay_port;
      cfg.device_id = options_.device_id;
      cfg.alias = options_.alias;
      cfg.target_id = params.target_id;
      cfg.password = params.password;
      cfg.codec = options_.codec;
      // 按 Web 实际需要的帧率协商，被控端据此降帧编码，避免解码端空转。
      cfg.fps = options_.fps;
      cfg.quality = options_.quality;
      cfg.render = false;

      auto runner = std::make_unique<ClientRunner>(cfg);
      runner->set_log_callback([](const std::string& tag, const std::string& level,
                                  const std::string& msg) {
        if (level == "ERROR") RC_LOGE("web", tag << ": " << msg);
        else if (level == "WARN") RC_LOGW("web", tag << ": " << msg);
        else RC_LOGI("web", tag << ": " << msg);
      });
      runner->set_status_callback([this](const std::string& state) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        state_ = state;
      });
      runner->set_frame_callback([this](const uint8_t* bgra, int width, int height,
                                        int stride) {
        on_frame(bgra, width, height, stride);
      });

      {
        std::lock_guard<std::mutex> lock(runner_mutex_);
        runner_ = runner.get();
      }
      const int result = runner->run();
      {
        std::lock_guard<std::mutex> lock(runner_mutex_);
        runner_ = nullptr;
      }
      if (stopping_.load()) break;

      // 期间被断开或更换了目标：丢弃旧结果，按新参数重建会话。
      {
        std::lock_guard<std::mutex> lock(session_mutex_);
        if (!configured_ || session_epoch_ != epoch) continue;
      }

      // 无人使用时暂停会话，直到新请求唤醒，避免解码链路空转。
      if (steady_ms() - last_activity_ms_.load() > kIdlePauseMs) {
        {
          std::lock_guard<std::mutex> lock(state_mutex_);
          state_ = "paused";
        }
        RC_LOGI("web", "无活动超过 15 秒，暂停视频会话");
        std::unique_lock<std::mutex> lock(pause_mutex_);
        while (!stopping_.load() &&
               steady_ms() - last_activity_ms_.load() > kIdlePauseMs) {
          pause_cv_.wait_for(lock, std::chrono::seconds(3));
        }
        RC_LOGI("web", "检测到新请求，恢复视频会话");
        continue;  // 立即重建会话，不走重试延迟
      }

      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        state_ = result == 3 ? "auth_failed" : "retrying";
      }
      RC_LOGW("web", "会话结束 rc=" << result << "，3 秒后重试");
      for (int i = 0; i < 30 && !stopping_.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    }
  }

  bool handle_input(const std::string& body) {
    json::Value value;
    try {
      value = json::Value::parse(body);
    } catch (const std::exception&) {
      return false;
    }
    const json::Value* type_field = value.get("type");
    if (!type_field) return false;
    const std::string type = type_field->as_string();

    auto number = [&value](const char* key, int fallback) {
      const json::Value* field = value.get(key);
      return field ? static_cast<int>(field->as_int(fallback)) : fallback;
    };

    InputEvent event;
    if (type == "move") {
      event.type = MsgType::InputMouseMove;
      event.x = number("x", 0);
      event.y = number("y", 0);
    } else if (type == "button") {
      event.type = MsgType::InputMouseButton;
      event.button = number("button", 0);
      event.state = number("state", 0);
      event.x = number("x", 0);
      event.y = number("y", 0);
      if (event.button <= 0) return false;
    } else if (type == "wheel") {
      event.type = MsgType::InputMouseWheel;
      event.delta_x = number("dx", 0);
      event.delta_y = number("dy", 0);
      if (event.delta_x == 0 && event.delta_y == 0) return true;
    } else if (type == "key") {
      event.type = MsgType::InputKey;
      event.key = number("key", 0);
      event.state = number("state", 0);
      if (event.key == 0) return false;
    } else {
      return false;
    }

    std::lock_guard<std::mutex> lock(runner_mutex_);
    if (!runner_) return false;
    runner_->send_input(event);
    return true;
  }

  std::string status_json() {
    std::string state;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      state = state_;
    }

    SessionParams params;
    bool connected = false;
    {
      std::lock_guard<std::mutex> lock(session_mutex_);
      connected = configured_;
      params = session_;
    }

    Snapshot snap;
    const bool has_frame = frames_.latest(snap);
    const int64_t age = frames_.age_ms();

    json::Value out = json::Value::make_object();
    out.set("connected", connected);
    out.set("target", params.target_id);
    out.set("relay", params.relay_host + ":" + std::to_string(params.relay_port));
    out.set("relay_default", options_.relay_host + ":" +
                                 std::to_string(options_.relay_port));
    out.set("state", state);
    out.set("streaming", state == "streaming" && has_frame);
    out.set("width", has_frame ? snap.width : 0);
    out.set("height", has_frame ? snap.height : 0);
    out.set("age_ms", static_cast<int64_t>(age < 0 ? 999999 : age));
    out.set("fps", measured_fps_.load());
    out.set("fps_target", options_.fps);
    out.set("viewers", viewers_.load());
    out.set("frames", static_cast<int64_t>(frames_published_.load()));
    return out.dump();
  }

 private:
  // 解码线程回调：按 --fps 抽帧后只做一次拷贝，重活交给编码线程。
  void on_frame(const uint8_t* bgra, int width, int height, int stride) {
    if (!bgra || width <= 0 || height <= 0 || stride < width * 4) return;

    const auto now = std::chrono::steady_clock::now();
    const auto interval =
        std::chrono::milliseconds(1000 / std::max(1, options_.fps));
    // 被控端已按目标帧率推流时不再二次抽帧：留 10% 容差，
    // 否则网络/编码抖动会让"刚好早到"的帧被丢掉，实际帧率反而变低。
    if (pushed_once_ && now - last_push_ < interval * 9 / 10) return;
    last_push_ = now;
    pushed_once_ = true;
    raw_.push(bgra, width, height, stride);
  }

  void encoder_loop() {
    uint64_t seq = 0;
    std::vector<uint8_t> buffer;
    int width = 0;
    int height = 0;

    auto publish_and_count = [&](std::shared_ptr<const std::vector<uint8_t>> jpeg) {
      frames_.publish(std::move(jpeg), width, height);
      frames_published_.fetch_add(1);
      ++window_frames_;
      const auto now = std::chrono::steady_clock::now();
      if (now - fps_window_ >= std::chrono::seconds(1)) {
        measured_fps_.store(window_frames_);
        window_frames_ = 0;
        fps_window_ = now;
      }
    };

    while (!raw_.stopped()) {
      // 无人使用超过阈值则停止会话，run() 检测到后进入暂停等待。
      if (steady_ms() - last_activity_ms_.load() > kIdlePauseMs) {
        std::lock_guard<std::mutex> lock(runner_mutex_);
        if (runner_) runner_->stop();
      }
      if (!raw_.wait_next(seq, buffer, width, height, 500)) continue;

      // 无人观看时降到 2fps，仅维持 /snapshot 可用，避免空转。
      const auto now_tp = std::chrono::steady_clock::now();
      if (viewers_.load() == 0 && encoded_once_ &&
          now_tp - last_encode_tp_ < std::chrono::milliseconds(500)) {
        continue;
      }

      // 帧去重：画面与上一帧完全一致时复用 JPEG，跳过编码。
      // 远程桌面大量时间静止，能省掉绝大部分 JPEG 开销；memcmp 比编码便宜约 50 倍。
      if (last_jpeg_ && width == last_width_ && height == last_height_ &&
          last_bgra_.size() == buffer.size() &&
          std::memcmp(last_bgra_.data(), buffer.data(), buffer.size()) == 0) {
        publish_and_count(last_jpeg_);
        continue;
      }

      if (!encoder_ || encoder_width_ != width || encoder_height_ != height) {
        encoder_.reset();
        std::string error;
        std::unique_ptr<IVideoEncoder> encoder = create_encoder("jpeg", &error);
        if (!encoder ||
            !encoder->open(width, height, std::max(1, options_.fps),
                           options_.quality, &error)) {
          RC_LOGW("web", "JPEG 编码器不可用: " << error);
          continue;
        }
        encoder_ = std::move(encoder);
        encoder_width_ = width;
        encoder_height_ = height;
        RC_LOGI("web", "JPEG 编码 " << width << "x" << height
                                    << " @" << std::max(1, options_.fps) << "fps");
      }

      const std::size_t row = static_cast<std::size_t>(width) * 4;
      frame_.pixels = std::move(buffer);
      frame_.width = width;
      frame_.height = height;
      frame_.stride = static_cast<int>(row);
      frame_.format = PixelFormat::kBGRA;
      frame_.capture_ts_us = static_cast<int64_t>(now_us());

      EncodedFrame encoded;
      std::string error;
      const bool ok = encoder_->encode(frame_, encoded, &error);
      buffer = std::move(frame_.pixels);
      if (!ok) {
        RC_LOGW("web", "JPEG 编码失败: " << error);
        continue;
      }

      auto jpeg = std::make_shared<const std::vector<uint8_t>>(std::move(encoded.data));
      // 保存原始帧用于下次去重比较，以及可复用的 JPEG。
      last_bgra_ = buffer;
      last_width_ = width;
      last_height_ = height;
      last_jpeg_ = jpeg;
      last_encode_tp_ = now_tp;
      encoded_once_ = true;
      publish_and_count(std::move(jpeg));
    }
  }

  Options options_;
  FrameStore frames_;
  RawFrameSlot raw_;

  // 网页提交的会话参数；configured_ 为 false 时 run() 在条件变量上等待。
  std::mutex session_mutex_;
  std::condition_variable session_cv_;
  SessionParams session_;
  bool configured_ = false;
  uint64_t session_epoch_ = 0;

  std::mutex runner_mutex_;
  ClientRunner* runner_ = nullptr;
  std::atomic<bool> stopping_{false};

  std::mutex state_mutex_;
  std::string state_ = "connecting";

  // 解码线程写，编码线程读，只通过 RawFrameSlot 交接。
  std::chrono::steady_clock::time_point last_push_{};
  bool pushed_once_ = false;

  // 以下只属于编码线程，独占访问，不需要加锁。
  std::thread encoder_thread_;
  std::unique_ptr<IVideoEncoder> encoder_;
  int encoder_width_ = 0;
  int encoder_height_ = 0;
  CapturedFrame frame_;

  // 编码线程独占：帧去重与无人降频。
  std::vector<uint8_t> last_bgra_;
  int last_width_ = 0;
  int last_height_ = 0;
  std::shared_ptr<const std::vector<uint8_t>> last_jpeg_;
  std::chrono::steady_clock::time_point last_encode_tp_{};
  bool encoded_once_ = false;

  std::chrono::steady_clock::time_point fps_window_{};
  int window_frames_ = 0;

  std::atomic<int> measured_fps_{0};
  std::atomic<uint64_t> frames_published_{0};
  std::atomic<int> viewers_{0};

  std::atomic<uint64_t> last_activity_ms_{0};
  std::mutex pause_mutex_;
  std::condition_variable pause_cv_;
};

void serve_stream(Gateway& gateway, TcpSocket& socket) {
  static const char* kHead =
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
      "Cache-Control: no-store, no-cache, must-revalidate\r\n"
      "Pragma: no-cache\r\n"
      "Connection: close\r\n\r\n";
  if (!socket.send_all(kHead, std::strlen(kHead))) return;

  uint64_t seq = 0;
  char part[160];
  while (!gateway.stopping()) {
    Snapshot snap;
    // 500ms 超时让线程能及时感知网关退出。
    if (!gateway.frames().wait_next(seq, snap, 500)) continue;
    seq = snap.seq;
    if (!snap.jpeg || snap.jpeg->empty()) continue;

    const int length = std::snprintf(
        part, sizeof(part),
        "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %zu\r\n\r\n",
        snap.jpeg->size());
    if (!socket.send_all(part, static_cast<std::size_t>(length))) return;
    if (!socket.send_all(snap.jpeg->data(), snap.jpeg->size())) return;
    if (!socket.send_all("\r\n", 2)) return;
  }
}

void bind_routes(HttpServer& server, Gateway& gateway) {
  auto page_handler = [&gateway](const HttpRequest&, TcpSocket& socket) {
    gateway.note_activity();
    return http_write_response(socket, 200, "text/html; charset=utf-8",
                               web_page_html());
  };
  server.route("/", page_handler);
  server.route("/index.html", page_handler);

  server.route("/stream", [&gateway](const HttpRequest&, TcpSocket& socket) {
    gateway.note_activity();
    gateway.add_viewer(1);
    serve_stream(gateway, socket);
    gateway.add_viewer(-1);
    return false;  // 流结束即关闭连接
  });

  server.route("/snapshot", [&gateway](const HttpRequest&, TcpSocket& socket) {
    gateway.note_activity();
    Snapshot snap;
    if (!gateway.frames().latest(snap) || !snap.jpeg || snap.jpeg->empty()) {
      return http_write_response(socket, 404, "text/plain; charset=utf-8",
                                 "no frame yet");
    }
    std::string head =
        "HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\nContent-Length: " +
        std::to_string(snap.jpeg->size()) +
        "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
    if (!socket.send_all(head.data(), head.size())) return false;
    return socket.send_all(snap.jpeg->data(), snap.jpeg->size());
  });

  server.route("/status", [&gateway](const HttpRequest&, TcpSocket& socket) {
    gateway.note_activity();
    return http_write_response(socket, 200, "application/json",
                               gateway.status_json());
  });

  server.route("/input", [&gateway](const HttpRequest& request, TcpSocket& socket) {
    gateway.note_activity();
    const bool ok = gateway.handle_input(request.body);
    return http_write_response(socket, 200, "application/json",
                               ok ? "{\"ok\":true}" : "{\"ok\":false}");
  });

  // 网页提交被控端信息后建立会话；密码只保存在内存中，断开即清空。
  server.route("/api/connect", [&gateway](const HttpRequest& request, TcpSocket& socket) {
    gateway.note_activity();
    bool ok = false;
    std::string error = "请求格式错误";
    try {
      const json::Value value = json::Value::parse(request.body);
      auto text = [&value](const char* key) {
        const json::Value* field = value.get(key);
        return field ? field->as_string() : std::string();
      };
      ok = gateway.connect_session(text("relay"), text("target"), text("password"),
                                   error);
    } catch (const std::exception&) {
      ok = false;
    }
    json::Value out = json::Value::make_object();
    out.set("ok", ok);
    if (!ok) out.set("error", error);
    return http_write_response(socket, 200, "application/json", out.dump());
  });

  server.route("/api/disconnect", [&gateway](const HttpRequest&, TcpSocket& socket) {
    gateway.note_activity();
    gateway.disconnect_session();
    return http_write_response(socket, 200, "application/json", "{\"ok\":true}");
  });
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!parse_options(argc, argv, options)) {
    print_usage();
    return 1;
  }

  net_init();

  Gateway gateway(options);
  HttpServer server;
  bind_routes(server, gateway);

  std::string error;
  if (!server.listen(options.http_port, &error)) {
    RC_LOGE("web", error);
    return 1;
  }

  gateway.start();
  std::thread([&gateway] { gateway.run(); }).detach();

  // 兼容旧用法：命令行显式给出目标时启动即连接；否则等待浏览器提交。
  if (!options.target_id.empty() && !options.password.empty()) {
    std::string err;
    if (!gateway.connect_session("", options.target_id, options.password, err)) {
      RC_LOGW("web", "命令行指定的会话无效：" << err);
    }
  }

  if (options.target_id.empty()) {
    RC_LOGI("web", "网页控制台 http://127.0.0.1:" << options.http_port
                                << "  等待浏览器填写被控端信息");
  } else {
    RC_LOGI("web", "网页控制台 http://127.0.0.1:" << options.http_port
                                << "  目标=" << options.target_id << "  中继="
                                << options.relay_host << ":" << options.relay_port);
  }

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  // 监听线程阻塞在 accept 上，用单独的线程轮询退出标志后关闭监听。
  std::thread([&server] {
    while (!g_interrupted.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    server.stop();
  }).detach();

  server.run();
  // 在主线程里收尾，确保编码线程 join 完成后再析构 Gateway。
  RC_LOGI("web", "正在退出…");
  gateway.stop();
  return 0;
}
