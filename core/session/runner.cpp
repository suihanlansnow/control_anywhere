#include "session/runner.h"

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

  // B5: H.264 编码器直接吃 NV12，省掉 8MB 回读和 CPU 逐像素转换。
  if (cfg_.codec == "h264" && !capture->set_output_format(PixelFormat::kNV12)) {
    log_("INFO", "host", "capture cannot output nv12, using bgra");
  }

  std::unique_ptr<IVideoEncoder> encoder = create_encoder(cfg_.codec, &error);
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
  if (!encoder->open(width, height, fps, cfg_.quality, &error)) {
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
    while (true) {
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
    if (!encoder->encode(last_frame, encoded, &enc_error)) {
      log_("WARN", "host", "encode failed: " + enc_error);
      continue;
    }

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
        guard.move_to(SessionState::Ready);
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
