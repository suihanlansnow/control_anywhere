#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "base/json.h"
#include "base/log.h"
#include "crypto/device_identity.h"
#include "crypto/hash.h"
#include "net/connection.h"
#include "net/socket.h"
#include "protocol/protocol.h"

using namespace rc;

namespace {

struct Peer {
  std::shared_ptr<Connection> conn;
  std::string device_id;
  std::string alias;
  // "host"（被控端）或 "controller"（主控端）。只有被控端强制设备身份。
  std::string role = "controller";
  std::string pub_key;
  // 非空表示已下发注册挑战、等待 RegisterProof。
  std::string pending_nonce;
};

struct Session {
  std::shared_ptr<Peer> host;
  std::shared_ptr<Peer> controller;
};

// 已下发邀请、等待被控端接受/拒绝的会话（有时限，避免主控端断线后残留）。
struct PendingSession {
  std::shared_ptr<Peer> controller;
  uint64_t created_ms = 0;
};

std::mutex g_mutex;
std::map<std::string, std::shared_ptr<Peer>> g_devices;
std::map<uint64_t, Session> g_sessions;
std::map<uint64_t, PendingSession> g_pending;
std::atomic<uint64_t> g_next_session{1000};
// 启动参数指定，之后只读；随 RegisterAck 下发给被控端和主控端。
std::vector<std::string> g_ice_servers;
// 非空表示所有入站连接都要求 TLS（PEM 证书与私钥）。
std::string g_tls_cert;
std::string g_tls_key;
// device_id -> 首次注册时固定的公钥（base64 DER）；用于阻止顶号。
std::map<std::string, std::string> g_device_keys;
// 身份公钥库文件；为空表示只在内存中固定（进程重启后失效）。
std::string g_device_db_path = "relay-devices.json";
// 下发给主控端的中继地址；为空时用主控端实际连接到的本机地址。
std::string g_advertise_host;
// 邀请等待被控端响应的默认超时，可用 --session-ttl 覆盖。
constexpr uint64_t kDefaultPendingTtlMs = 60000;
uint64_t g_pending_ttl_ms = kDefaultPendingTtlMs;

json::Value object() { return json::Value::make_object(); }

uint64_t monotonic_ms() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

std::string random_nonce() {
  std::random_device device;
  std::vector<uint8_t> bytes(32);
  for (auto& byte : bytes) byte = static_cast<uint8_t>(device());
  return base64_encode(bytes);
}

std::string json_string(const json::Value& value, const char* key,
                        const std::string& fallback = "") {
  const json::Value* field = value.get(key);
  return field ? field->as_string(fallback) : fallback;
}

json::Value parse_payload(const DecodedFrame& frame, const char* tag) {
  try {
    return json::Value::parse(
        std::string(frame.payload.begin(), frame.payload.end()));
  } catch (const std::exception& e) {
    RC_LOGW(tag, "bad json payload: " << e.what());
    return object();
  }
}

void send_error(const std::shared_ptr<Peer>& peer, const std::string& code,
                const std::string& message) {
  json::Value payload = object();
  payload.set("code", code);
  payload.set("message", message);
  peer->conn->send_json(MsgType::Error, 0, payload);
}

void send_register_ack(const std::shared_ptr<Peer>& peer, bool ok,
                       const std::string& reason, bool verified) {
  json::Value ack = object();
  ack.set("ok", ok);
  ack.set("device_id", peer->device_id);
  if (!ok) {
    ack.set("reason", reason);
  } else {
    ack.set("verified", verified);
    if (!g_ice_servers.empty()) {
      json::Value ice = object();
      json::Value servers = json::Value::make_array();
      for (const auto& uri : g_ice_servers) servers.push(json::Value(uri));
      ice.set("servers", servers);
      ack.set("ice", ice);
    }
  }
  peer->conn->send_json(MsgType::RegisterAck, 0, ack);
}

bool save_device_db(std::string* error) {
  if (g_device_db_path.empty()) return true;

  json::Value root = object();
  json::Value devices = json::Value::make_array();
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (const auto& entry : g_device_keys) {
      json::Value item = object();
      item.set("id", entry.first);
      item.set("key", entry.second);
      devices.push(item);
    }
  }
  root.set("version", 1);
  root.set("devices", devices);

  const std::filesystem::path path(g_device_db_path);
  std::error_code ec;
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path(), ec);
  }
  const std::string temp = g_device_db_path + ".tmp";
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out) {
      if (error) *error = "无法写入 " + temp;
      return false;
    }
    out << root.dump();
    if (!out.good()) {
      if (error) *error = "写入 " + temp + " 失败";
      return false;
    }
  }
  std::error_code rename_ec;
  std::filesystem::rename(temp, path, rename_ec);
  if (rename_ec) {
    // Windows 上目标已存在时 rename 会失败，先删除再重试。
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::rename(temp, path, rename_ec);
  }
  if (rename_ec) {
    std::error_code ignored;
    std::filesystem::remove(temp, ignored);
    if (error) *error = "无法替换 " + g_device_db_path + ": " + rename_ec.message();
    return false;
  }
  return true;
}

void load_device_db() {
  if (g_device_db_path.empty()) return;
  std::ifstream in(g_device_db_path, std::ios::binary);
  if (!in) return;
  try {
    const std::string text((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    const json::Value root = json::Value::parse(text);
    const json::Value* devices = root.get("devices");
    if (!devices || !devices->is_array()) return;
    for (std::size_t i = 0; i < devices->size(); ++i) {
      const json::Value& item = devices->at(i);
      const json::Value* id = item.get("id");
      const json::Value* key = item.get("key");
      if (id && key) g_device_keys[id->as_string()] = key->as_string();
    }
    RC_LOGI("relay", "device identity db loaded: " << g_device_keys.size()
                                                   << " device(s) from "
                                                   << g_device_db_path);
  } catch (const std::exception& e) {
    RC_LOGW("relay", "device identity db unreadable: " << e.what());
  }
}

// 首次注册时固定公钥；之后同一 device_id 只接受同一公钥，其他一律拒绝。
bool pin_device_key(const std::string& device_id, const std::string& pub_key,
                    std::string& reason) {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_device_keys.find(device_id);
    if (it != g_device_keys.end()) {
      if (it->second != pub_key) {
        reason = "device_id_taken";
        return false;
      }
      return true;
    }
    g_device_keys[device_id] = pub_key;
  }

  std::string error;
  if (!save_device_db(&error)) RC_LOGW("relay", "device identity db save failed: " << error);
  return true;
}

void finish_registration(const std::shared_ptr<Peer>& peer, bool verified) {
  std::shared_ptr<Peer> previous;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(peer->device_id);
    if (it != g_devices.end() && it->second.get() != peer.get()) previous = it->second;
    g_devices[peer->device_id] = peer;
  }
  if (previous) {
    // 同一身份重新注册（常见于断线重连）：断开旧连接，旧线程退出时会清理残留会话。
    RC_LOGI("relay", "device " << peer->device_id
                               << " re-registered, closing previous connection");
    previous->conn->close();
  }

  send_register_ack(peer, true, "", verified);
  RC_LOGI("relay", "device registered: " << peer->device_id << " (" << peer->alias
                                         << ", " << peer->role
                                         << (verified ? ", verified" : "") << ")");
}

void handle_register(const std::shared_ptr<Peer>& peer, const DecodedFrame& frame) {
  json::Value request = parse_payload(frame, "relay");
  const std::string device_id = json_string(request, "device_id");
  if (device_id.empty()) {
    send_error(peer, "bad_register", "device_id is required");
    return;
  }

  peer->device_id = device_id;
  peer->alias = json_string(request, "alias", device_id);
  peer->role = json_string(request, "role", "controller");
  peer->pub_key = json_string(request, "pub_key");

  if (peer->role == "host") {
    if (!DeviceIdentity::available()) {
      // 该构建没有 mbed TLS：无法校验签名，保留旧行为但明确告警。
      RC_LOGW("relay", "built without mbed TLS: host " << device_id
                                                      << " registered without identity");
      finish_registration(peer, false);
      return;
    }
    if (peer->pub_key.empty()) {
      send_register_ack(peer, false, "device_identity_required", false);
      RC_LOGW("relay", "host " << device_id << " rejected: no device identity");
      return;
    }
    peer->pending_nonce = random_nonce();
    json::Value challenge = object();
    challenge.set("device_id", device_id);
    challenge.set("nonce", peer->pending_nonce);
    peer->conn->send_json(MsgType::RegisterChallenge, 0, challenge);
    RC_LOGI("relay", "host " << device_id << " challenged for identity");
    return;
  }

  // 主控端不参与 device_id 抢注，直接注册。
  finish_registration(peer, false);
}

void handle_register_proof(const std::shared_ptr<Peer>& peer,
                           const DecodedFrame& frame) {
  if (peer->pending_nonce.empty() || peer->device_id.empty()) {
    send_register_ack(peer, false, "unexpected_proof", false);
    return;
  }

  json::Value request = parse_payload(frame, "relay");
  const std::string device_id = json_string(request, "device_id");
  const std::string pub_key = json_string(request, "pub_key");
  const std::string signature = json_string(request, "signature");

  if (device_id != peer->device_id || pub_key != peer->pub_key || signature.empty()) {
    peer->pending_nonce.clear();
    send_register_ack(peer, false, "invalid_proof", false);
    RC_LOGW("relay", "invalid register proof from " << device_id);
    return;
  }

  const std::string message =
      "rc-register-v1|" + device_id + "|" + peer->pending_nonce;
  std::string error;
  if (!DeviceIdentity::verify(pub_key, message, signature, &error)) {
    peer->pending_nonce.clear();
    send_register_ack(peer, false, "invalid_signature", false);
    RC_LOGW("relay", "device " << device_id << " signature rejected: " << error);
    return;
  }
  peer->pending_nonce.clear();

  std::string reason;
  if (!pin_device_key(device_id, pub_key, reason)) {
    send_register_ack(peer, false, reason, false);
    RC_LOGE("relay", "device " << device_id << " rejected: " << reason
                               << " (another key already owns this id)");
    return;
  }

  finish_registration(peer, true);
}

void handle_connect(const std::shared_ptr<Peer>& peer, const DecodedFrame& frame) {
  json::Value request = parse_payload(frame, "relay");
  const std::string target_id = json_string(request, "target_id");

  std::shared_ptr<Peer> host;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(target_id);
    if (it != g_devices.end()) host = it->second;
  }

  if (!host) {
    json::Value response = object();
    response.set("ok", false);
    response.set("reason", "target offline");
    peer->conn->send_json(MsgType::ConnectRsp, 0, response);
    RC_LOGW("relay", "connect to unknown device " << target_id);
    return;
  }

  const uint64_t session_id = g_next_session.fetch_add(1);
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_pending[session_id] = PendingSession{peer, monotonic_ms()};
  }

  json::Value invite = object();
  invite.set("session_id", static_cast<int64_t>(session_id));
  json::Value from = object();
  from.set("id", peer->device_id);
  from.set("alias", peer->alias);
  invite.set("from", from);
  if (const json::Value* want = request.get("want")) invite.set("want", *want);

  host->conn->send_json(MsgType::SessionInvite, session_id, invite);
  RC_LOGI("relay", "session " << session_id << " invited: " << peer->device_id
                              << " -> " << target_id);
}

void handle_accept(const std::shared_ptr<Peer>& peer, const DecodedFrame& frame) {
  const uint64_t session_id = frame.session_id;
  std::shared_ptr<Peer> controller;
  bool expired = false;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_pending.find(session_id);
    if (it != g_pending.end()) {
      expired = monotonic_ms() - it->second.created_ms >= g_pending_ttl_ms;
      if (!expired) controller = it->second.controller;
      g_pending.erase(it);
    }
    if (controller) {
      Session session;
      session.host = peer;
      session.controller = controller;
      g_sessions[session_id] = session;
    }
  }

  if (expired) {
    // 邀请已过期但被控端此时才接受：直接告诉被控端会话无效，不再通知主控端。
    json::Value response = object();
    response.set("ok", false);
    response.set("reason", "session_expired");
    peer->conn->send_json(MsgType::ConnectRsp, session_id, response);
    RC_LOGW("relay", "accept for expired session " << session_id);
    return;
  }
  if (!controller) {
    RC_LOGW("relay", "accept for unknown session " << session_id);
    return;
  }

  json::Value response = object();
  response.set("ok", true);
  response.set("session_id", static_cast<int64_t>(session_id));
  json::Value relay_info = object();
  // 优先使用显式配置的对外地址；否则回填主控端实际连接到的本机地址，
  // 避免多网卡/局域网部署时下发 127.0.0.1 导致后续连接失败。
  std::string relay_host = g_advertise_host;
  if (relay_host.empty()) relay_host = controller->conn->local_address();
  if (relay_host.empty()) relay_host = "127.0.0.1";
  relay_info.set("host", relay_host);
  relay_info.set("media_port", static_cast<int64_t>(kDefaultMediaPort));
  response.set("relay", relay_info);
  response.set("expire", 3600);
  controller->conn->send_json(MsgType::ConnectRsp, session_id, response);
  RC_LOGI("relay", "session " << session_id << " accepted, relay host " << relay_host);
}

void handle_reject(const std::shared_ptr<Peer>& peer, const DecodedFrame& frame) {
  const uint64_t session_id = frame.session_id;
  std::shared_ptr<Peer> controller;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_pending.find(session_id);
    if (it != g_pending.end()) {
      controller = it->second.controller;
      g_pending.erase(it);
    }
  }
  if (!controller) return;

  json::Value reason_payload = parse_payload(frame, "relay");
  const json::Value* reason_field = reason_payload.get("reason");
  const std::string reason =
      reason_field ? reason_field->as_string("rejected") : "rejected";

  json::Value response = object();
  response.set("ok", false);
  response.set("reason", reason);
  controller->conn->send_json(MsgType::ConnectRsp, session_id, response);
  RC_LOGI("relay", "session " << session_id << " rejected by host");
}

// 清理超过 kPendingTtlMs 仍未被被控端响应的邀请，并通知主控端超时。
void sweep_pending_sessions() {
  struct Expired {
    std::shared_ptr<Connection> conn;
    uint64_t session_id;
  };
  std::vector<Expired> expired;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    const uint64_t now = monotonic_ms();
    for (auto it = g_pending.begin(); it != g_pending.end();) {
      if (now - it->second.created_ms < g_pending_ttl_ms) {
        ++it;
        continue;
      }
      expired.push_back({it->second.controller->conn, it->first});
      it = g_pending.erase(it);
    }
  }

  for (const Expired& item : expired) {
    json::Value response = object();
    response.set("ok", false);
    response.set("reason", "session_expired");
    item.conn->send_json(MsgType::ConnectRsp, item.session_id, response);
  }
  if (!expired.empty()) {
    RC_LOGW("relay", "expired " << expired.size() << " pending session(s)");
  }
}

void forward_session_frame(const std::shared_ptr<Peer>& peer, const DecodedFrame& frame) {
  std::shared_ptr<Connection> target;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_sessions.find(frame.session_id);
    if (it == g_sessions.end()) return;
    if (it->second.host.get() == peer.get()) {
      target = it->second.controller->conn;
    } else {
      target = it->second.host->conn;
    }
    if (frame.type == MsgType::SessionClose) {
      g_sessions.erase(it);
    }
  }

  if (!target) return;
  target->send(frame.type, frame.session_id, frame.payload.data(),
               frame.payload.size(), frame.seq, frame.flags);
}

void cleanup(const std::shared_ptr<Peer>& peer) {
  struct Notify {
    std::shared_ptr<Connection> conn;
    uint64_t session_id;
  };
  std::vector<Notify> notifications;

  {
    std::lock_guard<std::mutex> lock(g_mutex);

    for (auto it = g_devices.begin(); it != g_devices.end();) {
      if (it->second.get() == peer.get()) {
        it = g_devices.erase(it);
      } else {
        ++it;
      }
    }

    for (auto it = g_pending.begin(); it != g_pending.end();) {
      if (it->second.controller.get() == peer.get()) {
        it = g_pending.erase(it);
      } else {
        ++it;
      }
    }

    for (auto it = g_sessions.begin(); it != g_sessions.end();) {
      if (it->second.host.get() == peer.get() ||
          it->second.controller.get() == peer.get()) {
        const bool host_side = it->second.host.get() == peer.get();
        std::shared_ptr<Connection> other =
            host_side ? it->second.controller->conn : it->second.host->conn;
        notifications.push_back({other, it->first});
        it = g_sessions.erase(it);
      } else {
        ++it;
      }
    }
  }

  for (const Notify& item : notifications) {
    json::Value payload = object();
    payload.set("reason", "peer disconnected");
    item.conn->send_json(MsgType::SessionClose, item.session_id, payload);
  }

  // 被同身份新连接替换时旧连接也会走到这里，此时设备仍在线，不打印离线日志。
  bool still_registered = false;
  if (!peer->device_id.empty()) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(peer->device_id);
    still_registered = (it != g_devices.end() && it->second.get() == peer.get());
  }
  if (!peer->device_id.empty() && !still_registered) {
    RC_LOGI("relay", "device offline: " << peer->device_id);
  }
}

void handle_client(std::shared_ptr<Connection> conn) {
  auto peer = std::make_shared<Peer>();
  peer->conn = conn;

  DecodedFrame frame;
  while (conn->recv(frame)) {
    switch (frame.type) {
      case MsgType::Register:
        handle_register(peer, frame);
        break;
      case MsgType::RegisterProof:
        handle_register_proof(peer, frame);
        break;
      case MsgType::ConnectReq:
        handle_connect(peer, frame);
        break;
      case MsgType::SessionAccept:
        handle_accept(peer, frame);
        break;
      case MsgType::SessionReject:
        handle_reject(peer, frame);
        break;
      case MsgType::Ping:
        conn->send_json(MsgType::Pong, 0, object());
        break;
      default:
        if (frame.session_id != 0) {
          forward_session_frame(peer, frame);
        } else {
          RC_LOGW("relay", "unhandled frame " << msg_type_name(frame.type));
        }
        break;
    }
  }

  cleanup(peer);
}

}  // namespace

int main(int argc, char** argv) {
  uint16_t port = kDefaultControlPort;
  auto with_scheme = [](const std::string& value, const std::string& scheme) {
    if (value.rfind("stun:", 0) == 0 || value.rfind("turn:", 0) == 0 ||
        value.rfind("turns:", 0) == 0) {
      return value;
    }
    return scheme + value;
  };
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const bool has_next = (i + 1) < argc;
    if (arg == "--port" && has_next) {
      port = static_cast<uint16_t>(std::atoi(argv[++i]));
    } else if (arg == "--stun" && has_next) {
      g_ice_servers.push_back(with_scheme(argv[++i], "stun:"));
    } else if (arg == "--turn" && has_next) {
      g_ice_servers.push_back(with_scheme(argv[++i], "turn:"));
    } else if (arg == "--tls-cert" && has_next) {
      g_tls_cert = argv[++i];
    } else if (arg == "--tls-key" && has_next) {
      g_tls_key = argv[++i];
    } else if (arg == "--device-db" && has_next) {
      g_device_db_path = argv[++i];
    } else if (arg == "--advertise-host" && has_next) {
      g_advertise_host = argv[++i];
    } else if (arg == "--session-ttl" && has_next) {
      const int seconds = std::atoi(argv[++i]);
      g_pending_ttl_ms = static_cast<uint64_t>(seconds < 5 ? 5 : seconds) * 1000;
    } else if (arg == "--no-device-db") {
      g_device_db_path.clear();
    } else {
      std::fprintf(stderr,
                   "usage: %s [--port N] [--stun host:port] "
                   "[--turn [user:pass@]host:port]\n"
                   "       [--tls-cert relay.pem --tls-key relay-key.pem]\n"
                   "       [--device-db relay-devices.json | --no-device-db]\n"
                   "       [--advertise-host <对外地址>] [--session-ttl 60]\n",
                   argv[0]);
      return 2;
    }
  }

  if (g_tls_cert.empty() != g_tls_key.empty()) {
    std::fprintf(stderr, "--tls-cert 与 --tls-key 必须同时提供\n");
    return 2;
  }

  load_device_db();

  std::string error;
  TcpListener listener = TcpListener::listen(port, &error);
  if (!listener.valid()) {
    RC_LOGE("relay", error);
    return 1;
  }

  RC_LOGI("relay", "listening on tcp/" << port
                                   << (g_tls_cert.empty() ? " (plaintext)" : " (TLS)"));
  // 邀请超时清理线程：每 5 秒扫描一次待响应的会话。
  std::thread([] {
    while (true) {
      std::this_thread::sleep_for(std::chrono::seconds(5));
      sweep_pending_sessions();
    }
  }).detach();
  while (true) {
    TcpSocket socket = listener.accept();
    if (!socket.valid()) continue;
    std::thread([sock = std::move(socket)]() mutable {
      if (!g_tls_cert.empty()) {
        std::string error;
        if (!sock.start_tls_server(g_tls_cert, g_tls_key, &error)) {
          RC_LOGW("relay", "TLS handshake failed: " << error);
          return;
        }
      }
      auto conn = std::make_shared<Connection>(std::move(sock));
      handle_client(conn);
    }).detach();
  }
}
