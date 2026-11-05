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

  std::unique_ptr<IVideoEncoder> encoder = create_encoder(cfg_.codec, &error);
  if (!encoder) {
    log_("ERROR", "host", "encoder unavailable: " + error);
    return 0;
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

  log_("INFO", "host",
       std::string("capture=") + capture->name() + " codec=" + encoder->name() + " " +
           std::to_string(width) + "x" + std::to_string(height) + " fps=" + std::to_string(fps));
  if (status_cb_) status_cb_("streaming");

  std::mutex slot_mutex;
  std::condition_variable slot_cv;
  CapturedFrame latest_frame;
  bool slot_has_frame = false;
  int captured_count = 0;
  int overwritten_count = 0;
  std::atomic<bool> stop_threads{false};

  std::thread capture_thread([&]() mutable {
    int frame_interval_ms = std::max(1, 1000 / fps);
    auto next_tick = std::chrono::steady_clock::now();
    while (!stop_flag_.load() && !stop_threads.load()) {
      CapturedFrame frame;
      if (!capture->grab(frame, frame_interval_ms)) continue;
      {
        std::lock_guard<std::mutex> lock(slot_mutex);
        if (slot_has_frame) ++overwritten_count;
        latest_frame = std::move(frame);
        slot_has_frame = true;
        ++captured_count;
      }
      slot_cv.notify_one();
      next_tick += std::chrono::milliseconds(frame_interval_ms);
      std::this_thread::sleep_until(next_tick);
    }
    capture->stop();
  });

  int sent_count = 0;
  uint64_t sent_bytes = 0;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(5000);

  while (!stop_flag_.load()) {
    CapturedFrame frame;
    {
      std::unique_lock<std::mutex> lock(slot_mutex);
      if (!slot_cv.wait_for(lock, std::chrono::milliseconds(500),
                            [&]() { return slot_has_frame || stop_threads.load(); })) {
        if (sent_count > 0 && std::chrono::steady_clock::now() > deadline) break;
        continue;
      }
      if (!slot_has_frame) break;
      frame = std::move(latest_frame);
      slot_has_frame = false;
    }

    EncodedFrame encoded;
    std::string enc_error;
    if (!encoder->encode(frame, encoded, &enc_error)) {
      log_("WARN", "host", "encode failed: " + enc_error);
      continue;
    }

    VideoFrameMeta meta;
    meta.codec = (cfg_.codec == "h264" || cfg_.codec == "h264-soft") ? 1 : 4;
    meta.width = frame.width;
    meta.height = frame.height;
    meta.keyframe = encoded.keyframe;
    meta.capture_ts_us = frame.capture_ts_us;
    std::vector<uint8_t> payload = build_video_payload(meta, encoded.data.data(), encoded.data.size());

    if (!conn.send(MsgType::VideoFrame, session_id, payload.data(), payload.size())) {
      log_("WARN", "host", "send failed, breaking stream");
      break;
    }
    ++sent_count;
    sent_bytes += payload.size();
    deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(5000);

    if (progress_cb_) progress_cb_(sent_count, static_cast<int>(sent_bytes));
    if (cfg_.max_frames > 0 && sent_count >= cfg_.max_frames) break;
  }

  stop_threads.store(true);
  slot_cv.notify_all();
  if (capture_thread.joinable()) capture_thread.join();

  encoder->close();
  log_("INFO", "host", "frames_sent=" + std::to_string(sent_count) +
                         " overwritten=" + std::to_string(overwritten_count));
  return sent_count;
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

  std::unique_ptr<IVideoDecoder> decoder;
  int frames_decoded = 0;
  uint64_t total_decode_us = 0;

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

        if (!decoder) {
          std::string dec_error;
          decoder = create_decoder(&dec_error);
          if (decoder) decoder->open(&dec_error);
        }
        DecodedImage picture;
        std::string dec_error;
        auto t0 = now_us();
        if (decoder) {
          std::size_t data_len = frame.payload.size() >= kVideoHeaderBytes
              ? frame.payload.size() - kVideoHeaderBytes : 0;
          const uint8_t* data = data_len > 0 ? frame.payload.data() + kVideoHeaderBytes : nullptr;
          if (data && decoder->decode(data, data_len, meta.capture_ts_us, picture, &dec_error)) {
            total_decode_us += now_us() - t0;
            ++frames_decoded;
          }
        }

        ++frames_received;
        bytes_received += frame.payload.size();
        if (progress_cb_) progress_cb_(frames_received, static_cast<int>(bytes_received));

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

  if (frames_decoded > 0) {
    log_("INFO", "ctrl", "frames_decoded=" + std::to_string(frames_decoded) +
                           " avg_decode=" + std::to_string(total_decode_us / frames_decoded) + "us");
  }
  return 0;
}

}  // namespace rc
