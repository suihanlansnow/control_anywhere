#include "session/session.h"

#include "base/log.h"
#include "crypto/hash.h"

namespace rc {
namespace {

bool allowed(SessionState from, SessionState to) {
  switch (from) {
    case SessionState::Idle:
      return to == SessionState::Registered || to == SessionState::Closed;
    case SessionState::Registered:
      return to == SessionState::Connecting || to == SessionState::Invited ||
             to == SessionState::Closing || to == SessionState::Closed;
    case SessionState::Connecting:
    case SessionState::Invited:
      return to == SessionState::Authing || to == SessionState::Closing ||
             to == SessionState::Closed;
    case SessionState::Authing:
      return to == SessionState::Ready || to == SessionState::Closing ||
             to == SessionState::Closed;
    case SessionState::Ready:
      return to == SessionState::Streaming || to == SessionState::Closing ||
             to == SessionState::Closed;
    case SessionState::Streaming:
      return to == SessionState::Closing || to == SessionState::Closed;
    case SessionState::Closing:
      return to == SessionState::Closed;
    case SessionState::Closed:
      return false;
  }
  return false;
}

}  // namespace

const char* state_name(SessionState state) {
  switch (state) {
    case SessionState::Idle: return "Idle";
    case SessionState::Registered: return "Registered";
    case SessionState::Connecting: return "Connecting";
    case SessionState::Invited: return "Invited";
    case SessionState::Authing: return "Authing";
    case SessionState::Ready: return "Ready";
    case SessionState::Streaming: return "Streaming";
    case SessionState::Closing: return "Closing";
    case SessionState::Closed: return "Closed";
  }
  return "Unknown";
}

bool can_transition(SessionState from, SessionState to) {
  return allowed(from, to);
}

bool SessionGuard::move_to(SessionState next, const char* tag) {
  if (!allowed(state_, next)) {
    RC_LOGW(tag, "illegal state transition " << state_name(state_) << " -> "
                                             << state_name(next));
    return false;
  }
  RC_LOGD(tag, state_name(state_) << " -> " << state_name(next));
  state_ = next;
  return true;
}

std::vector<uint8_t> make_nonce() {
  return random_bytes(32);
}

std::string password_proof(const std::string& password,
                           const std::vector<uint8_t>& nonce) {
  // M0 简化实现：key = sha256(password)，proof = HMAC-SHA256(key, nonce)。
  // 正式版替换为 PBKDF2-HMAC-SHA256(高迭代) + Ed25519 设备签名。
  const std::vector<uint8_t> key = sha256(password);
  const std::vector<uint8_t> digest = hmac_sha256(key, nonce);
  return base64_encode(digest);
}

bool verify_password_proof(const std::string& password,
                           const std::vector<uint8_t>& nonce,
                           const std::string& proof_base64) {
  return password_proof(password, nonce) == proof_base64;
}

}  // namespace rc
