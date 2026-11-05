#ifdef _WIN32
// 在这些平台头文件之前定义，避免 windows.h 的 min/max 宏破坏 std::min/std::max。
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
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
#include "codec/decoder.h"
#include "render/renderer.h"
#ifdef _WIN32
#include "codec/color_convert.h"
#include "render/window_renderer.h"
#endif
#ifdef __linux__
#include "render/sdl_renderer.h"
#endif
#include "crypto/hash.h"
#include "net/connection.h"
#include "net/socket.h"
#include "protocol/protocol.h"
#include "protocol/video_payload.h"
#include "session/session.h"

using namespace rc;

namespace {

struct Options {
  std::string command;
  std::string target_id;
  std::string device_id;
  std::string alias;
  std::string password;
  std::string relay_host = "127.0.0.1";
#ifdef __linux__
  std::string capture = "x11";
#else
  std::string capture = "dxgi";
#endif
  std::string codec = "h264";
  uint16_t relay_port = kDefaultControlPort;
  int frames = 60;
  int fps = 60;
  int monitor = 0;
  int quality = 75;
  bool render = true;
  std::string dump_frame;
};

void print_usage() {
  std::printf(
      "用法:\n"
      "  rc-relay [--port 21116]\n"
      "  rc-cli host    --id <设备号> [--alias 名称] --password <密码> "
      "[--relay 127.0.0.1:21116]\n"
      "                 [--capture dxgi|synthetic] [--codec h264|jpeg] [--fps 60] "
      "[--quality 75] [--monitor 0] [--frames 60]\n"
      "  rc-cli connect <目标设备号> --password <密码> "
      "[--relay 127.0.0.1:21116] [--frames 30]\n"
      "                 [--render|--no-render] [--dump-frame 首帧.jpg]\n"
      "  rc-cli selftest\n");
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
  if (argc < 2) return false;
  opts.command = argv[1];

  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    auto take_value = [&](std::string& out) {
      if (i + 1 >= argc) return false;
      out = argv[++i];
      return true;
    };

    if (arg == "--id") {
      if (!take_value(opts.device_id)) return false;
    } else if (arg == "--alias") {
      if (!take_value(opts.alias)) return false;
    } else if (arg == "--password") {
      if (!take_value(opts.password)) return false;
    } else if (arg == "--relay") {
      std::string value;
      if (!take_value(value)) return false;
      split_host_port(value, opts.relay_host, opts.relay_port);
    } else if (arg == "--frames") {
      std::string value;
      if (!take_value(value)) return false;
      opts.frames = std::atoi(value.c_str());
    } else if (arg == "--fps") {
      std::string value;
      if (!take_value(value)) return false;
      opts.fps = std::atoi(value.c_str());
    } else if (arg == "--capture") {
      if (!take_value(opts.capture)) return false;
    } else if (arg == "--codec") {
      if (!take_value(opts.codec)) return false;
    } else if (arg == "--monitor") {
      std::string value;
      if (!take_value(value)) return false;
      opts.monitor = std::atoi(value.c_str());
    } else if (arg == "--quality") {
      std::string value;
      if (!take_value(value)) return false;
      opts.quality = std::atoi(value.c_str());
    } else if (arg == "--render") {
      opts.render = true;
    } else if (arg == "--no-render") {
      opts.render = false;
    } else if (arg == "--dump-frame") {
      if (!take_value(opts.dump_frame)) return false;
    } else if (!arg.empty() && arg[0] != '-') {
      opts.target_id = arg;
    } else {
      return false;
    }
  }
  return true;
}

void write_u16(std::vector<uint8_t>& buffer, std::size_t offset, uint16_t value) {
  buffer[offset + 0] = static_cast<uint8_t>(value & 0xFF);
  buffer[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
}

void write_u32(std::vector<uint8_t>& buffer, std::size_t offset, uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    buffer[offset + i] = static_cast<uint8_t>((value >> (i * 8)) & 0xFF);
  }
}

uint16_t read_u16(const std::vector<uint8_t>& buffer, std::size_t offset) {
  return static_cast<uint16_t>(buffer[offset] | (buffer[offset + 1] << 8));
}

json::Value object() { return json::Value::make_object(); }

json::Value parse_payload(const DecodedFrame& frame, const char* tag) {
  try {
    return json::Value::parse(
        std::string(frame.payload.begin(), frame.payload.end()));
  } catch (const std::exception& e) {
    RC_LOGW(tag, "bad json payload: " << e.what());
    return object();
  }
}

std::vector<uint8_t> make_mouse_move(uint16_t x, uint16_t y, uint8_t display_id) {
  std::vector<uint8_t> payload(10, 0);
  write_u32(payload, 0, now_ms());
  write_u16(payload, 4, x);
  write_u16(payload, 6, y);
  payload[8] = display_id;
  payload[9] = 0;
  return payload;
}

std::vector<uint8_t> make_mouse_button(uint8_t button, uint8_t state, uint16_t x,
                                       uint16_t y) {
  std::vector<uint8_t> payload(12, 0);
  write_u32(payload, 0, now_ms());
  payload[4] = button;
  payload[5] = state;
  write_u16(payload, 6, x);
  write_u16(payload, 8, y);
  payload[10] = 0;
  payload[11] = 0;
  return payload;
}

TcpSocket open_relay(const Options& opts, const char* tag) {
  std::string error;
  TcpSocket socket = TcpSocket::connect(opts.relay_host, opts.relay_port, &error);
  if (!socket.valid()) {
    RC_LOGE(tag, error);
    return socket;
  }
  RC_LOGI(tag, "connected to relay " << opts.relay_host << ":" << opts.relay_port);
  return socket;
}

bool send_register(Connection& conn, const Options& opts, const char* tag) {
  json::Value request = object();
  request.set("device_id", opts.device_id);
  request.set("alias", opts.alias.empty() ? opts.device_id : opts.alias);
#ifdef __linux__
  request.set("os", "linux");
#else
  request.set("os", "windows");
#endif
  request.set("os_ver", "m0");
  request.set("app", "0.1.0");
  json::Value caps = object();
  json::Value codec = json::Value::make_array();
  codec.push(opts.codec);
  caps.set("codec", codec);
  caps.set("input", true);
  request.set("caps", caps);
  RC_LOGI(tag, "register device " << opts.device_id);
  return conn.send_json(MsgType::Register, 0, request);
}

int stream_frames(Connection& conn, const Options& opts, uint64_t session_id) {
  std::string error;
  std::unique_ptr<ICaptureSource> capture = create_capture_source(opts.capture, &error);
  if (!capture) {
    RC_LOGW("host", error << ", fallback to synthetic");
    capture = create_capture_source("synthetic", &error);
  }
  if (!capture) {
    RC_LOGE("host", "no capture source available");
    return 0;
  }
  if (!capture->start(opts.monitor, &error)) {
    RC_LOGE("host", "capture start failed: " << error << ", fallback to synthetic");
    capture = create_capture_source("synthetic", &error);
    if (!capture || !capture->start(0, &error)) {
      RC_LOGE("host", "synthetic capture start failed: " << error);
      return 0;
    }
  }

  // H.264 编码器直接吃 NV12：让采集端在 GPU 上转换，省掉 8MB 回读和 CPU 逐像素转换。
  if (opts.codec == "h264" && !capture->set_output_format(PixelFormat::kNV12)) {
    RC_LOGW("host", "capture cannot output nv12, using bgra");
  }

  std::unique_ptr<IVideoEncoder> encoder = create_encoder(opts.codec, &error);
  if (!encoder) {
    RC_LOGE("host", "encoder unavailable: " << error);
    return 0;
  }

  // 采集与编码共享同一块 D3D11 设备，硬编才能直接吃 GPU 纹理（零回读）。
  if (void* device = capture->native_device(); device != nullptr) {
    if (!encoder->set_d3d_device(device)) {
      RC_LOGW("host", "encoder rejected shared d3d device");
    }
  }

  // 采集线程按这个开关决定交纹理还是回读 NV12；编码器 open 后才能确定。
  // manager 必须先于开关生效：采集侧的 GPU 调用要和 MFT 串行，否则会卡死。
  std::atomic<bool> texture_output{false};
  std::atomic<void*> device_manager{nullptr};

  const int fps = opts.fps > 0 ? opts.fps : 60;
  const int grab_timeout = std::min(1000 / fps, 33);
  const auto frame_interval = std::chrono::milliseconds(1000 / fps);

  RC_LOGI("host", "capture=" << capture->name() << " codec=" << opts.codec
                             << " fps=" << fps << " quality=" << opts.quality);

  // 采集线程只做 grab，编码线程只做 encode + send。
  // 队列深度 1 且覆盖旧帧：编码跟不上时丢旧帧而不是排队，保证延迟不累积。
  struct FrameSlot {
    std::mutex mutex;
    std::condition_variable cv;
    CapturedFrame frame;
    bool has_frame = false;
    bool stop = false;
    uint64_t captured = 0;
    uint64_t overwritten = 0;
  };
  FrameSlot slot;

  // 发送线程：把编码与网络发送解耦，避免编码被 send 阻塞拖慢节拍。
  // 队列深度 1 且覆盖旧帧：网络跟不上时丢旧帧，保证延迟不累积。
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

  std::thread capture_thread([&slot, &texture_output, &device_manager,
                              source = std::move(capture),
                              monitor = opts.monitor, grab_timeout]() mutable {
    std::string start_error;
    if (!source->start(monitor, &start_error)) {
      RC_LOGW("host", "capture start failed: " << start_error << ", fallback to synthetic");
      source = create_capture_source("synthetic", &start_error);
      if (!source || !source->start(0, &start_error)) {
        RC_LOGE("host", "synthetic capture start failed: " << start_error);
        std::lock_guard<std::mutex> lock(slot.mutex);
        slot.stop = true;
        slot.cv.notify_all();
        return;
      }
    }
    RC_LOGI("host", "capture thread started: " << source->name());
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
      if (!source->grab(frame, grab_timeout)) continue;
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
    while (true) {
      SendTask task;
      {
        std::unique_lock<std::mutex> lock(send_slot.mutex);
        send_slot.cv.wait(lock, [&] { return send_slot.has_frame || send_slot.stop; });
        if (send_slot.stop && !send_slot.has_frame) break;
        task = std::move(send_slot.task);
        send_slot.has_frame = false;
      }
      if (!conn.send_video_frame(task.session_id, task.meta, task.data.data(),
                                 task.data.size(), task.frame_id)) {
        RC_LOGW("host", "send frame failed");
        send_failed = true;
        break;
      }
      sent_count.fetch_add(1);
      sent_bytes.fetch_add(task.data.size());
    }
  });

  bool encoder_open = false;
  bool failed = false;
  int sent = 0;
  uint32_t frame_id = 0;
  uint64_t total_bytes = 0;
  uint64_t total_latency_us = 0;
  uint64_t first_ts = 0;
  uint64_t last_ts = 0;
  CapturedFrame last_frame;
  bool has_last = false;

  auto next_slot = std::chrono::steady_clock::now();
  bool running = true;
  while (running && sent < opts.frames && !failed) {
    bool fresh = false;
    if (!has_last) {
      // 首帧：直接等，不受节拍约束。
      std::unique_lock<std::mutex> lock(slot.mutex);
      slot.cv.wait_for(lock, std::chrono::milliseconds(200),
                       [&slot] { return slot.has_frame || slot.stop; });
      if (slot.stop) running = false;
      if (slot.has_frame) {
        last_frame = std::move(slot.frame);
        slot.has_frame = false;
        has_last = true;
        fresh = true;
      }
      if (!running) break;
      if (!has_last) continue;
      next_slot = std::chrono::steady_clock::now() + frame_interval;
    } else {
      // 等到发帧时点；等待期间只要来了新画面就立刻替换，避免空等一整帧。
      for (;;) {
        const auto now_point = std::chrono::steady_clock::now();
        if (now_point >= next_slot) break;
        auto remain =
            std::chrono::duration_cast<std::chrono::milliseconds>(next_slot - now_point);
        if (remain > std::chrono::milliseconds(4)) remain = std::chrono::milliseconds(4);
        std::unique_lock<std::mutex> lock(slot.mutex);
        slot.cv.wait_for(lock, remain, [&slot] { return slot.has_frame || slot.stop; });
        if (slot.stop) {
          running = false;
          break;
        }
        if (slot.has_frame) {
          last_frame = std::move(slot.frame);
          slot.has_frame = false;
          fresh = true;
        }
      }
      if (!running) break;
      next_slot += frame_interval;
      if (next_slot < std::chrono::steady_clock::now()) {
        next_slot = std::chrono::steady_clock::now();
      }
    }

    if (!fresh) {
      // 画面静止时 DXGI 不产新帧，复用上一帧并按当前时间重新计时，保证目标帧率。
      last_frame.capture_ts_us = static_cast<int64_t>(now_us());
    }
    const CapturedFrame& captured = last_frame;

    if (!encoder_open) {
      if (!encoder->open(captured.width, captured.height, fps, opts.quality, &error)) {
        RC_LOGE("host", "encoder open failed: " << error);
        failed = true;
        break;
      }
      encoder_open = true;
      RC_LOGI("host", "encoder=" << encoder->name() << " " << captured.width << "x"
                                 << captured.height);
    }
    // manager 必须先于开关生效；直通也可能中途失效，每轮同步一次。
    device_manager.store(encoder->native_device_manager());
    texture_output.store(encoder->accepts_texture_input());

    EncodedFrame encoded;
    error.clear();
    if (!encoder->encode(captured, encoded, &error)) {
      if (!error.empty()) RC_LOGW("host", "encode failed: " << error);
      continue;
    }

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

    ++sent;
    total_latency_us += encoded.encode_ts_us - encoded.capture_ts_us;
    if (first_ts == 0) first_ts = encoded.capture_ts_us;
    last_ts = encoded.capture_ts_us;

    if (sent % 20 == 0 || sent == opts.frames) {
      RC_LOGI("host", "frame " << frame_id << " " << encoded.width << "x"
                               << encoded.height << " latency="
                               << (encoded.encode_ts_us - encoded.capture_ts_us) << "us"
                               << (encoded.keyframe ? " [key]" : ""));
    }
    if (send_failed.load()) {
      failed = true;
      break;
    }
  }

  {
    std::lock_guard<std::mutex> lock(slot.mutex);
    slot.stop = true;
  }
  slot.cv.notify_all();
  capture_thread.join();

  // 通知发送线程退出并等待清空。
  {
    std::lock_guard<std::mutex> lock(send_slot.mutex);
    send_slot.stop = true;
  }
  send_slot.cv.notify_all();
  send_thread.join();

  if (encoder_open) encoder->close();

  if (sent > 0) {
    const double seconds = std::max(0.001, (last_ts - first_ts) / 1e6);
    RC_LOGI("host", "streamed " << sent << " frames, sent=" << sent_count.load()
                                << " avg=" << static_cast<int>(sent / seconds)
                                << "fps, "
                                << static_cast<int>(sent_bytes.load() * 8 / seconds / 1000)
                                << "kbps, avg_latency=" << (total_latency_us / sent)
                                << "us, captured=" << slot.captured
                                << " dropped=" << slot.overwritten);
  }
  return sent;
}

int run_selftest() {
  int failures = 0;
  auto check = [&](const char* name, bool ok) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
  };

  check("sha256(abc)",
        hex_encode(sha256("abc")) ==
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

  std::vector<uint8_t> key(20, 0x0b);
  std::vector<uint8_t> message{'H', 'i', ' ', 'T', 'h', 'e', 'r', 'e'};
  check("hmac-sha256 rfc4231 case1",
        hex_encode(hmac_sha256(key, message)) ==
            "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");

  std::vector<uint8_t> hello{0x68, 0x65, 0x6c, 0x6c, 0x6f};
  check("base64 encode", base64_encode(hello) == "aGVsbG8=");
  check("base64 roundtrip", base64_decode("aGVsbG8=") == hello);

  FrameHeader header;
  header.type = static_cast<uint8_t>(MsgType::Register);
  header.session_id = 42;
  header.seq = 7;
  std::vector<uint8_t> payload{1, 2, 3, 4, 5};
  const std::vector<uint8_t> frame = encode_frame(header, payload.data(), payload.size());
  FrameHeader decoded;
  const bool frame_ok = decode_frame_header(frame.data(), frame.size(), decoded) &&
                        decoded.session_id == 42 && decoded.seq == 7 &&
                        decoded.payload_len == payload.size();
  check("frame encode/decode", frame_ok);

  const std::vector<uint8_t> nonce = make_nonce();
  const std::string proof = password_proof("123456", nonce);
  check("password proof ok", verify_password_proof("123456", nonce, proof));
  check("password proof rejects wrong",
        !verify_password_proof("654321", nonce, proof));

  std::printf("\n%s (%d failure%s)\n", failures == 0 ? "SELFTEST PASS" : "SELFTEST FAIL",
              failures, failures == 1 ? "" : "s");
  return failures == 0 ? 0 : 1;
}

int run_host(const Options& opts) {
  TcpSocket socket = open_relay(opts, "host");
  if (!socket.valid()) return 2;

  Connection conn(std::move(socket));
  conn.set_recv_timeout_ms(30000);
  if (!send_register(conn, opts, "host")) return 2;

  SessionGuard guard;
  uint64_t session_id = 0;
  std::vector<uint8_t> nonce;
  int frames_sent = 0;
  int input_events = 0;
  bool running = true;

  while (running) {
    DecodedFrame frame;
    if (!conn.recv(frame)) break;

    switch (frame.type) {
      case MsgType::RegisterAck: {
        json::Value payload = parse_payload(frame, "host");
        if (!payload.get("ok") || !payload.get("ok")->as_bool()) {
          RC_LOGE("host", "register rejected");
          return 2;
        }
        guard.move_to(SessionState::Registered, "host");
        RC_LOGI("host", "online, waiting for controller");
        break;
      }
      case MsgType::SessionInvite: {
        session_id = frame.session_id;
        json::Value payload = parse_payload(frame, "host");
        std::string from = "unknown";
        if (const json::Value* from_field = payload.get("from")) {
          if (const json::Value* id_field = from_field->get("id")) {
            from = id_field->as_string("unknown");
          }
        }

        guard.move_to(SessionState::Invited, "host");
        json::Value accept = object();
        accept.set("session_id", static_cast<int64_t>(session_id));
        conn.send_json(MsgType::SessionAccept, session_id, accept);

        nonce = make_nonce();
        json::Value challenge = object();
        challenge.set("session_id", static_cast<int64_t>(session_id));
        challenge.set("nonce", base64_encode(nonce));
        challenge.set("method", "password");
        conn.send_json(MsgType::AuthChallenge, session_id, challenge);
        guard.move_to(SessionState::Authing, "host");
        RC_LOGI("host", "session " << session_id << " invited by " << from);
        break;
      }
      case MsgType::AuthResponse: {
        json::Value payload = parse_payload(frame, "host");
        const std::string proof =
            payload.get("password_proof") ? payload.get("password_proof")->as_string() : "";
        const bool ok = verify_password_proof(opts.password, nonce, proof);

        json::Value result = object();
        result.set("ok", ok);
        if (!ok) result.set("reason", "bad password");
        conn.send_json(MsgType::AuthResult, session_id, result);

        if (!ok) {
          RC_LOGW("host", "authentication failed");
          guard.move_to(SessionState::Closing, "host");
          running = false;
          break;
        }

        guard.move_to(SessionState::Ready, "host");
        guard.move_to(SessionState::Streaming, "host");
        RC_LOGI("host", "authentication passed, starting capture/encode");
        frames_sent += stream_frames(conn, opts, session_id);
        break;
      }
      case MsgType::InputMouseMove: {
        ++input_events;
        if (frame.payload.size() >= 10) {
          const uint16_t x = read_u16(frame.payload, 4);
          const uint16_t y = read_u16(frame.payload, 6);
          RC_LOGI("host", "input #" << input_events << " move (" << x << "," << y << ")");
        }
        break;
      }
      case MsgType::InputMouseButton: {
        ++input_events;
        if (frame.payload.size() >= 12) {
          const uint8_t button = frame.payload[4];
          const uint8_t state = frame.payload[5];
          const uint16_t x = read_u16(frame.payload, 6);
          const uint16_t y = read_u16(frame.payload, 8);
          RC_LOGI("host", "input #" << input_events << " button=" << static_cast<int>(button)
                                    << " state=" << static_cast<int>(state) << " ("
                                    << x << "," << y << ")");
        }
        break;
      }
      case MsgType::SessionClose: {
        json::Value ack = object();
        ack.set("reason", "ack");
        conn.send_json(MsgType::SessionClose, session_id, ack);
        guard.move_to(SessionState::Closed, "host");
        RC_LOGI("host", "session closed by controller");
        running = false;
        break;
      }
      default:
        break;
    }
  }

  RC_LOGI("host", "frames=" << frames_sent << " input_events=" << input_events
                             << " final_state=" << state_name(guard.state()));
  return (frames_sent == opts.frames && input_events > 0) ? 0 : 2;
}

// 跨平台首帧落盘：复用 jpeg 编码器把解码后的画面写成 JPEG。
// Windows 解码器可能输出 NV12（GPU 直出路径），需先转 BGRA；
// Linux 解码器固定输出 BGRA，可直接编码。
void try_dump_first_frame(const Options& opts, DecodedImage& picture,
                          std::unique_ptr<IVideoEncoder>& dump_encoder,
                          bool& dumped) {
  if (opts.dump_frame.empty() || dumped) return;

  if (!dump_encoder) {
    std::string enc_error;
    dump_encoder = create_encoder("jpeg", &enc_error);
    if (dump_encoder &&
        !dump_encoder->open(picture.width, picture.height, 30, 90, &enc_error)) {
      RC_LOGW("ctrl", "dump encoder unavailable: " << enc_error);
      dump_encoder.reset();
    }
  }
  if (!dump_encoder) return;

  std::vector<uint8_t> raw;
  int raw_stride = picture.stride;
  if (picture.nv12) {
#ifdef _WIN32
    nv12_to_bgra(picture.pixels.data(), picture.width, picture.height,
                 picture.coded_height, picture.stride, raw);
    raw_stride = picture.width * 4;
#else
    RC_LOGW("ctrl", "nv12 dump not supported on this platform");
    return;
#endif
  } else {
    raw = std::move(picture.pixels);
  }

  CapturedFrame source;
  source.width = picture.width;
  source.height = picture.height;
  source.stride = raw_stride;
  source.capture_ts_us = static_cast<int64_t>(picture.timestamp_us);
  source.pixels = std::move(raw);
  EncodedFrame jpeg;
  std::string dump_error;
  if (dump_encoder->encode(source, jpeg, &dump_error)) {
    FILE* file = std::fopen(opts.dump_frame.c_str(), "wb");
    if (file != nullptr) {
      std::fwrite(jpeg.data.data(), 1, jpeg.data.size(), file);
      std::fclose(file);
      dumped = true;
      RC_LOGI("ctrl", "dumped first frame -> " << opts.dump_frame << " ("
                                              << jpeg.data.size() << "B)");
    } else {
      RC_LOGW("ctrl", "cannot write " << opts.dump_frame);
    }
  } else {
    RC_LOGW("ctrl", "dump encode failed: " << dump_error);
  }
}

int run_connect(const Options& opts) {
  if (opts.target_id.empty()) {
    RC_LOGE("ctrl", "target device id is required");
    return 1;
  }

  TcpSocket socket = open_relay(opts, "ctrl");
  if (!socket.valid()) return 2;

  Connection conn(std::move(socket));
  conn.set_recv_timeout_ms(10000);
  if (!send_register(conn, opts, "ctrl")) return 2;

  SessionGuard guard;
  uint64_t session_id = 0;
  std::vector<uint8_t> nonce;
  int frames_received = 0;
  uint64_t bytes_received = 0;
  uint64_t first_ts = 0;
  uint64_t last_ts = 0;
  bool running = true;

#ifdef _WIN32
  std::unique_ptr<IVideoDecoder> decoder;
  std::unique_ptr<IRenderer> renderer;
  std::unique_ptr<IVideoEncoder> dump_encoder;
  bool dumped = false;
  bool saw_h264 = false;
  int frames_decoded = 0;
  uint64_t total_decode_us = 0;
#endif
#ifdef __linux__
  std::unique_ptr<IVideoDecoder> decoder;
  std::unique_ptr<IRenderer> renderer;
  std::unique_ptr<IVideoEncoder> dump_encoder;
  bool dumped = false;
  bool saw_h264 = false;
  int frames_decoded = 0;
  uint64_t total_decode_us = 0;
#endif

  // 先等注册回执，再发起连接，保证状态机时序正确。
  while (true) {
    DecodedFrame frame;
    if (!conn.recv(frame)) {
      RC_LOGE("ctrl", "no register ack from relay");
      return 2;
    }
    if (frame.type == MsgType::RegisterAck) {
      json::Value payload = parse_payload(frame, "ctrl");
      if (!payload.get("ok") || !payload.get("ok")->as_bool()) {
        RC_LOGE("ctrl", "register rejected");
        return 2;
      }
      guard.move_to(SessionState::Registered, "ctrl");
      break;
    }
    if (frame.type == MsgType::Error) {
      RC_LOGE("ctrl", "relay error while registering");
      return 2;
    }
  }

  json::Value request = object();
  request.set("target_id", opts.target_id);
  request.set("mode", "control");
  json::Value want = object();
  want.set("codec", opts.codec);
  want.set("max_fps", opts.fps);
  want.set("max_width", 0);  // 0 = 由被控端按原生分辨率输出
  request.set("want", want);
  conn.send_json(MsgType::ConnectReq, 0, request);
  guard.move_to(SessionState::Connecting, "ctrl");
  RC_LOGI("ctrl", "requesting session with " << opts.target_id);

  while (running) {
    DecodedFrame frame;
    if (!conn.recv(frame)) {
      RC_LOGW("ctrl", "connection lost");
      break;
    }

    switch (frame.type) {
      case MsgType::ConnectRsp: {
        json::Value payload = parse_payload(frame, "ctrl");
        const json::Value* ok_field = payload.get("ok");
        if (!ok_field || !ok_field->as_bool()) {
          const json::Value* reason = payload.get("reason");
          RC_LOGE("ctrl", "connect rejected: "
                              << (reason ? reason->as_string("unknown") : "unknown"));
          return 2;
        }
        session_id = frame.session_id;
        RC_LOGI("ctrl", "session " << session_id << " accepted by host");
        break;
      }
      case MsgType::AuthChallenge: {
        json::Value payload = parse_payload(frame, "ctrl");
        const json::Value* nonce_field = payload.get("nonce");
        if (!nonce_field) {
          RC_LOGE("ctrl", "challenge without nonce");
          return 2;
        }
        nonce = base64_decode(nonce_field->as_string());
        guard.move_to(SessionState::Authing, "ctrl");

        json::Value response = object();
        response.set("password_proof", password_proof(opts.password, nonce));
        conn.send_json(MsgType::AuthResponse, session_id, response);
        RC_LOGI("ctrl", "password proof sent");
        break;
      }
      case MsgType::AuthResult: {
        json::Value payload = parse_payload(frame, "ctrl");
        const json::Value* ok_field = payload.get("ok");
        if (!ok_field || !ok_field->as_bool()) {
          RC_LOGE("ctrl", "authentication rejected");
          return 2;
        }
        guard.move_to(SessionState::Ready, "ctrl");
        guard.move_to(SessionState::Streaming, "ctrl");
        RC_LOGI("ctrl", "authenticated, sending input events");

        const std::vector<uint8_t> move = make_mouse_move(32000, 24000, 0);
        conn.send(MsgType::InputMouseMove, session_id, move.data(), move.size());

        const std::vector<uint8_t> down = make_mouse_button(1, 1, 32000, 24000);
        conn.send(MsgType::InputMouseButton, session_id, down.data(), down.size());

        const std::vector<uint8_t> up = make_mouse_button(1, 0, 32000, 24000);
        conn.send(MsgType::InputMouseButton, session_id, up.data(), up.size());
        RC_LOGI("ctrl", "input events sent");
        break;
      }
      case MsgType::VideoFrame: {
        VideoFrameMeta meta;
        if (!parse_video_payload(frame.payload, meta)) {
          RC_LOGW("ctrl", "malformed video frame");
          break;
        }
        ++frames_received;
        bytes_received += meta.data_len;
        if (first_ts == 0) first_ts = meta.capture_ts_us;
        last_ts = meta.capture_ts_us;

        bool format_ok = true;
        if (meta.codec == static_cast<uint8_t>(VideoCodec::Jpeg) && meta.data_len >= 2) {
          const uint8_t* data = frame.payload.data() + kVideoHeaderBytes;
          format_ok = data[0] == 0xFF && data[1] == 0xD8;
        }

        if (meta.keyframe || frames_received % 20 == 0) {
          RC_LOGI("ctrl", "video #" << meta.frame_id << " " << meta.width << "x"
                                    << meta.height << " "
                                    << codec_name(static_cast<VideoCodec>(meta.codec)) << " "
                                    << meta.data_len << "B"
                                    << " encode=" << (meta.encode_ts_us - meta.capture_ts_us)
                                    << "us"
                                    << (meta.keyframe ? " [key]" : "")
                                    << (format_ok ? "" : " [bad jpeg marker]"));
        }

#ifdef _WIN32
        if (meta.codec == static_cast<uint8_t>(VideoCodec::H264)) {
          saw_h264 = true;

          // 窗口消息必须在创建它的线程里泵，否则拖拽/关闭都不响应。
          if (renderer) {
            renderer->pump();
            if (!renderer->alive()) {
              RC_LOGW("ctrl", "window closed by user, stop rendering");
              renderer.reset();
            }
          }

          // 窗口必须在解码前建立：解码输出格式取决于窗口能否走 GPU 直出。
          if (opts.render && !renderer) {
            renderer = std::make_unique<WindowRenderer>();
            std::string render_error;
            if (!renderer->open("rc remote - " + opts.target_id,
                                static_cast<int>(meta.width),
                                static_cast<int>(meta.height), &render_error)) {
              RC_LOGE("ctrl", "window open failed: " << render_error);
              renderer.reset();
            }
          }

          if (!decoder) {
            decoder = create_decoder(nullptr);
            // swapchain 就绪时让解码器直接出 NV12，由 VideoProcessor 画进 back buffer，
            // 省掉回读 8MB BGRA 与 GDI 拷贝。
            if (renderer && renderer->gpu()) {
              decoder->set_output(IVideoDecoder::Output::kNv12);
            }
            std::string decode_error;
            if (!decoder->open(&decode_error)) {
              RC_LOGE("ctrl", "h264 decoder open failed: " << decode_error);
              decoder.reset();
            } else {
              RC_LOGI("ctrl", "decoder=" << decoder->name());
            }
          }

          if (decoder) {
            DecodedImage picture;
            std::string decode_error;
            const uint64_t begin_us = now_us();
            const uint8_t* data = frame.payload.data() + kVideoHeaderBytes;
            if (decoder->decode(data, meta.data_len,
                                static_cast<int64_t>(meta.capture_ts_us), picture,
                                &decode_error)) {
              ++frames_decoded;
              total_decode_us += now_us() - begin_us;

              if (renderer) {
                if (picture.nv12) {
                  renderer->present_nv12(picture.pixels.data(), picture.width,
                                         picture.height, picture.coded_height,
                                         picture.stride);
                } else {
                  renderer->present(picture.pixels.data(), picture.width,
                                    picture.height, picture.stride);
                }
              }

              // 首帧落盘，方便在无人值守的联调环境里直接验证解码画面。
              try_dump_first_frame(opts, picture, dump_encoder, dumped);
            } else if (!decode_error.empty() && frames_received <= 3) {
              RC_LOGW("ctrl", "decode failed: " << decode_error);
            }
          }
        }
#elif defined(__linux__)
        if (meta.codec == static_cast<uint8_t>(VideoCodec::H264)) {
          saw_h264 = true;

          if (renderer) {
            renderer->pump();
            if (!renderer->alive()) {
              RC_LOGW("ctrl", "window closed by user, stop rendering");
              renderer.reset();
            }
          }

          if (opts.render && !renderer) {
            renderer = std::make_unique<SdlRenderer>();
            std::string render_error;
            if (!renderer->open("rc remote - " + opts.target_id,
                                static_cast<int>(meta.width),
                                static_cast<int>(meta.height), &render_error)) {
              RC_LOGE("ctrl", "sdl window open failed: " << render_error);
              renderer.reset();
            }
          }

          if (!decoder) {
            decoder = create_decoder(nullptr);
            decoder->set_output(IVideoDecoder::Output::kBgra);
            std::string decode_error;
            if (!decoder->open(&decode_error)) {
              RC_LOGE("ctrl", "h264 decoder open failed: " << decode_error);
              decoder.reset();
            } else {
              RC_LOGI("ctrl", "decoder=" << decoder->name());
            }
          }

          if (decoder) {
            DecodedImage picture;
            std::string decode_error;
            const uint64_t begin_us = now_us();
            const uint8_t* data = frame.payload.data() + kVideoHeaderBytes;
            if (decoder->decode(data, meta.data_len,
                                static_cast<int64_t>(meta.capture_ts_us), picture,
                                &decode_error)) {
              ++frames_decoded;
              total_decode_us += now_us() - begin_us;

              if (renderer && !picture.nv12) {
                renderer->present(picture.pixels.data(), picture.width,
                                  picture.height, picture.stride);
              }

              // 首帧落盘，方便在无人值守的联调环境里直接验证解码画面。
              try_dump_first_frame(opts, picture, dump_encoder, dumped);
            } else if (!decode_error.empty() && frames_received <= 3) {
              RC_LOGW("ctrl", "decode failed: " << decode_error);
            }
          }
        }
#endif

        if (frames_received >= opts.frames) {
          json::Value close_payload = object();
          close_payload.set("reason", "m1 test done");
          conn.send_json(MsgType::SessionClose, session_id, close_payload);
          guard.move_to(SessionState::Closing, "ctrl");
          running = false;
        }
        break;
      }
      case MsgType::SessionClose:
        RC_LOGI("ctrl", "session closed by host");
        guard.move_to(SessionState::Closed, "ctrl");
        running = false;
        break;
      case MsgType::Error: {
        json::Value payload = parse_payload(frame, "ctrl");
        RC_LOGE("ctrl", "relay error: "
                            << (payload.get("message") ? payload.get("message")->as_string()
                                                       : "unknown"));
        return 2;
      }
      default:
        break;
    }
  }

  RC_LOGI("ctrl", "frames=" << frames_received << " final_state="
                             << state_name(guard.state()));
#if defined(_WIN32) || defined(__linux__)
  if (decoder) {
    RC_LOGI("ctrl", "decoded=" << frames_decoded << " avg_decode="
                               << (frames_decoded > 0 ? total_decode_us / frames_decoded : 0)
                               << "us");
  }
#endif
  if (frames_received > 0 && last_ts > first_ts) {
    const double seconds = (last_ts - first_ts) / 1e6;
    RC_LOGI("ctrl", "received " << bytes_received << "B in " << static_cast<int>(seconds * 1000)
                                << "ms, fps=" << static_cast<int>(frames_received / seconds)
                                << " kbps=" << static_cast<int>(bytes_received * 8 / seconds / 1000));
  }

  bool decoded_ok = true;
#if defined(_WIN32) || defined(__linux__)
  if (saw_h264) decoded_ok = frames_decoded > 0;
#endif
  return (frames_received >= opts.frames && decoded_ok) ? 0 : 2;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!parse_options(argc, argv, options)) {
    print_usage();
    return 1;
  }

  if (options.command == "selftest") return run_selftest();

  if (options.command == "host") {
    if (options.device_id.empty() || options.password.empty()) {
      print_usage();
      return 1;
    }
    return run_host(options);
  }

  if (options.command == "connect") {
    if (options.password.empty()) {
      print_usage();
      return 1;
    }
    if (options.device_id.empty()) options.device_id = "900000001";
    if (options.alias.empty()) options.alias = "controller";
    return run_connect(options);
  }

  print_usage();
  return 1;
}
