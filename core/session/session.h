#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rc {

enum class SessionState {
  Idle,
  Registered,
  Connecting,  // 主控端：已发 CONNECT_REQ，等待应答
  Invited,     // 被控端：已收到 SESSION_INVITE
  Authing,
  Ready,
  Streaming,
  Closing,
  Closed,
};

const char* state_name(SessionState state);

bool can_transition(SessionState from, SessionState to);

// 状态迁移守卫：非法迁移会被拒绝并记录，便于在联调阶段尽早暴露时序问题。
class SessionGuard {
 public:
  explicit SessionGuard(SessionState initial = SessionState::Idle)
      : state_(initial) {}

  SessionState state() const { return state_; }

  bool move_to(SessionState next, const char* tag = "session");

 private:
  SessionState state_;
};

// 鉴权：nonce 由被控端生成，主控端回 proof。
std::vector<uint8_t> make_nonce();
std::string password_proof(const std::string& password,
                           const std::vector<uint8_t>& nonce);
bool verify_password_proof(const std::string& password,
                           const std::vector<uint8_t>& nonce,
                           const std::string& proof_base64);

}  // namespace rc
