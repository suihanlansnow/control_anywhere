#include "session/runner.h"

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

namespace rc {

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

HostRunner::HostRunner(const SessionConfig& cfg) : cfg_(cfg) {}
HostRunner::~HostRunner() { stop(); }

void HostRunner::stop() { stop_flag_.store(true); }

void HostRunner::log_(const std::string& level, const std::string& tag, const std::string& msg) {
  if (log_cb_) {
    log_cb_(tag, level, msg);
  } else {
    if (level == "ERROR") RC_LOGE(tag, msg);
    else if (level == "WARN") RC_LOGW(tag, msg);
    else RC_LOGI(tag, msg);
  }
}

int HostRunner::stream_frames_(Connection& conn, uint64_t session_id) {
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
  int fps = cfg_.fps > 0 ? cfg_.fps : 60;
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

  const int frame_interval_ms = std::max(1, 1000 / fps);

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
      if (!conn.send_video_frame(task.session_id, task.meta, task.data.data(),
                                 task.data.size(), task.frame_id)) {
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

  while (!stop_flag_.load() && !send_failed.load()) {
    CapturedFrame frame;
    {
      std::unique_lock<std::mutex> lock(slot.mutex);
      // 等到发帧时点；等待期间只要来了新画面就立刻替换。
      for (;;) {
        auto now_point = std::chrono::steady_clock::now();
        if (now_point >= next_slot) break;
        auto remain = std::chrono::duration_cast<std::chrono::milliseconds>(next_slot - now_point);
        if (remain > std::chrono::milliseconds(4)) remain = std::chrono::milliseconds(4);
        slot.cv.wait_for(lock, remain, [&slot] { return slot.has_frame || slot.stop; });
        if (slot.stop) { stop_flag_.store(true); break; }
        if (slot.has_frame) {
          frame = std::move(slot.frame);
          slot.has_frame = false;
          continue;  // 继续等到节拍
        }
        // 画面静止时没新帧，直接跳到节拍点
        break;
      }
      if (slot.stop) break;
      if (frame.pixels.empty() && !frame.gpu_texture) continue;
    }
    next_slot += frame_interval;
    if (next_slot < std::chrono::steady_clock::now()) {
      next_slot = std::chrono::steady_clock::now();
    }

    EncodedFrame encoded;
    std::string enc_error;
    if (!encoder->encode(frame, encoded, &enc_error)) {
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

  encoder->close();
  log_("INFO", "host", "frames_encoded=" + std::to_string(encoded_count) +
                         " frames_sent=" + std::to_string(sent_count.load()) +
                         " captured=" + std::to_string(slot.captured) +
                         " dropped=" + std::to_string(slot.overwritten));
  return encoded_count;
}

int HostRunner::run() {
  std::string error;
  TcpSocket socket = TcpSocket::connect(cfg_.relay_host, cfg_.relay_port, &error);
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
  int frames_sent = 0;

  while (!stop_flag_.load()) {
    DecodedFrame frame;
    if (!conn.recv(frame)) break;

    switch (frame.type) {
      case MsgType::RegisterAck: {
        json::Value payload = parse_payload_cfg(frame);
        if (!payload.get("ok") || !payload.get("ok")->as_bool()) {
          log_("ERROR", "host", "register rejected");
          return 2;
        }
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
        if (!ok) result.set("reason", "bad password");
        conn.send_json(MsgType::AuthResult, session_id, result);

        if (!ok) {
          log_("WARN", "host", "authentication failed");
          guard.move_to(SessionState::Closing);
          if (status_cb_) status_cb_("auth_failed");
          return 3;
        }
        guard.move_to(SessionState::Ready);
        guard.move_to(SessionState::Streaming);
        log_("INFO", "host", "authentication passed, starting capture/encode");
        frames_sent = stream_frames_(conn, session_id);
        break;
      }
      case MsgType::SessionClose: {
        json::Value ack = make_object();
        ack.set("reason", "ack");
        conn.send_json(MsgType::SessionClose, session_id, ack);
        guard.move_to(SessionState::Closed);
        log_("INFO", "host", "session closed by controller");
        if (status_cb_) status_cb_("closed");
        return 0;
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

void ClientRunner::stop() { stop_flag_.store(true); }

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
  TcpSocket socket = TcpSocket::connect(cfg_.relay_host, cfg_.relay_port, &error);
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
  request.set("want", want);
  conn.send_json(MsgType::ConnectReq, 0, request);
  guard.move_to(SessionState::Connecting);
  log_("INFO", "ctrl", "requesting session with " + cfg_.target_id);

  // 第 4 点：解码放到独立线程，收包线程不再被解码耗时阻塞；
  // 队列深度 1，解码跟不上时覆盖旧帧而不是排队累积延迟。
  std::mutex decode_mutex;
  std::condition_variable decode_cv;
  std::vector<uint8_t> decode_payload;
  VideoFrameMeta decode_meta;
  bool decode_has_frame = false;
  std::atomic<bool> decode_stop{false};
  std::atomic<int> frames_decoded{0};
  std::atomic<uint64_t> total_decode_us{0};

  std::thread decode_thread([&]() {
    std::unique_ptr<IVideoDecoder> decoder;
    DecodedImage picture;
    while (true) {
      std::vector<uint8_t> payload;
      VideoFrameMeta meta;
      {
        std::unique_lock<std::mutex> lock(decode_mutex);
        decode_cv.wait(lock, [&] { return decode_has_frame || decode_stop.load(); });
        if (decode_stop.load() && !decode_has_frame) break;
        payload = std::move(decode_payload);
        meta = decode_meta;
        decode_has_frame = false;
      }

      const std::size_t data_len = payload.size() >= kVideoHeaderBytes
          ? payload.size() - kVideoHeaderBytes : 0;
      if (data_len == 0) continue;
      const uint8_t* data = payload.data() + kVideoHeaderBytes;

      if (!decoder) {
        std::string dec_error;
        decoder = create_decoder(&dec_error);
        if (!decoder) {
          log_("WARN", "ctrl", "decoder create failed: " + dec_error);
          continue;
        }
        decoder->set_output(IVideoDecoder::Output::kBgra);
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
          frame_cb_(picture.pixels.data(), picture.width, picture.height, picture.stride);
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

  while (!stop_flag_.load()) {
    DecodedFrame frame;
    if (!conn.recv(frame)) break;

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
        guard.move_to(SessionState::Ready);
        guard.move_to(SessionState::Streaming);
        log_("INFO", "ctrl", "authenticated");
        if (status_cb_) status_cb_("streaming");
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
