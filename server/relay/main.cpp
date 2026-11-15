#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "base/json.h"
#include "base/log.h"
#include "net/connection.h"
#include "net/socket.h"
#include "protocol/protocol.h"

using namespace rc;

namespace {

struct Peer {
  std::shared_ptr<Connection> conn;
  std::string device_id;
  std::string alias;
};

struct Session {
  std::shared_ptr<Peer> host;
  std::shared_ptr<Peer> controller;
};

std::mutex g_mutex;
std::map<std::string, std::shared_ptr<Peer>> g_devices;
std::map<uint64_t, Session> g_sessions;
std::map<uint64_t, std::shared_ptr<Peer>> g_pending;  // session_id -> controller
std::atomic<uint64_t> g_next_session{1000};
// 启动参数指定，之后只读；随 RegisterAck 下发给被控端和主控端。
std::vector<std::string> g_ice_servers;

json::Value object() { return json::Value::make_object(); }

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

void handle_register(const std::shared_ptr<Peer>& peer, const DecodedFrame& frame) {
  json::Value request = parse_payload(frame, "relay");
  const std::string device_id = json_string(request, "device_id");
  if (device_id.empty()) {
    send_error(peer, "bad_register", "device_id is required");
    return;
  }

  peer->device_id = device_id;
  peer->alias = json_string(request, "alias", device_id);
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_devices[device_id] = peer;
  }

  json::Value ack = object();
  ack.set("ok", true);
  ack.set("device_id", device_id);
  if (!g_ice_servers.empty()) {
    json::Value ice = object();
    json::Value servers = json::Value::make_array();
    for (const auto& uri : g_ice_servers) servers.push(json::Value(uri));
    ice.set("servers", servers);
    ack.set("ice", ice);
  }
  peer->conn->send_json(MsgType::RegisterAck, 0, ack);
  RC_LOGI("relay", "device registered: " << device_id << " (" << peer->alias << ")");
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
    g_pending[session_id] = peer;
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
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_pending.find(session_id);
    if (it != g_pending.end()) {
      controller = it->second;
      g_pending.erase(it);
    }
    if (controller) {
      Session session;
      session.host = peer;
      session.controller = controller;
      g_sessions[session_id] = session;
    }
  }

  if (!controller) {
    RC_LOGW("relay", "accept for unknown session " << session_id);
    return;
  }

  json::Value response = object();
  response.set("ok", true);
  response.set("session_id", static_cast<int64_t>(session_id));
  json::Value relay_info = object();
  relay_info.set("host", "127.0.0.1");
  relay_info.set("media_port", static_cast<int64_t>(kDefaultMediaPort));
  response.set("relay", relay_info);
  response.set("expire", 3600);
  controller->conn->send_json(MsgType::ConnectRsp, session_id, response);
  RC_LOGI("relay", "session " << session_id << " accepted");
}

void handle_reject(const std::shared_ptr<Peer>& peer, const DecodedFrame& frame) {
  const uint64_t session_id = frame.session_id;
  std::shared_ptr<Peer> controller;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_pending.find(session_id);
    if (it != g_pending.end()) {
      controller = it->second;
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
      if (it->second.get() == peer.get()) {
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

  if (!peer->device_id.empty()) {
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
    } else {
      std::fprintf(stderr,
                   "usage: %s [--port N] [--stun host:port] "
                   "[--turn [user:pass@]host:port]\n",
                   argv[0]);
      return 2;
    }
  }

  std::string error;
  TcpListener listener = TcpListener::listen(port, &error);
  if (!listener.valid()) {
    RC_LOGE("relay", error);
    return 1;
  }

  RC_LOGI("relay", "listening on tcp/" << port);
  while (true) {
    TcpSocket socket = listener.accept();
    if (!socket.valid()) continue;
    auto conn = std::make_shared<Connection>(std::move(socket));
    std::thread([conn] { handle_client(conn); }).detach();
  }
}
