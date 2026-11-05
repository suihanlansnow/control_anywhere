#include "protocol/protocol.h"

#include <cstring>

#include "base/log.h"

namespace rc {
namespace {

inline void write_u32(uint8_t* out, uint32_t value) {
  out[0] = static_cast<uint8_t>(value & 0xFF);
  out[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
  out[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
  out[3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

inline void write_u64(uint8_t* out, uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    out[i] = static_cast<uint8_t>((value >> (i * 8)) & 0xFF);
  }
}

inline uint32_t read_u32(const uint8_t* in) {
  return static_cast<uint32_t>(in[0]) | (static_cast<uint32_t>(in[1]) << 8) |
         (static_cast<uint32_t>(in[2]) << 16) |
         (static_cast<uint32_t>(in[3]) << 24);
}

inline uint64_t read_u64(const uint8_t* in) {
  uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value |= static_cast<uint64_t>(in[i]) << (i * 8);
  }
  return value;
}

}  // namespace

std::vector<uint8_t> encode_frame(const FrameHeader& header, const uint8_t* payload,
                                  std::size_t payload_len) {
  std::vector<uint8_t> out(kFrameHeaderSize + payload_len);
  out[0] = header.version;
  out[1] = header.type;
  out[2] = header.flags;
  out[3] = header.reserved;
  write_u32(out.data() + 4, header.msg_id);
  write_u64(out.data() + 8, header.session_id);
  write_u32(out.data() + 16, header.seq);
  write_u32(out.data() + 20, static_cast<uint32_t>(payload_len));
  if (payload_len > 0 && payload != nullptr) {
    std::memcpy(out.data() + kFrameHeaderSize, payload, payload_len);
  }
  return out;
}

bool decode_frame_header(const uint8_t* data, std::size_t available,
                         FrameHeader& out) {
  if (available < kFrameHeaderSize) return false;
  out.version = data[0];
  out.type = data[1];
  out.flags = data[2];
  out.reserved = data[3];
  out.msg_id = read_u32(data + 4);
  out.session_id = read_u64(data + 8);
  out.seq = read_u32(data + 16);
  out.payload_len = read_u32(data + 20);
  if (out.version != kProtocolVersion) {
    RC_LOGW("protocol", "unsupported version " << static_cast<int>(out.version));
    return false;
  }
  if (out.payload_len > kMaxFramePayload) {
    RC_LOGW("protocol", "payload too large: " << out.payload_len);
    return false;
  }
  return true;
}

const char* msg_type_name(MsgType type) {
  switch (type) {
    case MsgType::HelloReq: return "HELLO_REQ";
    case MsgType::HelloRsp: return "HELLO_RSP";
    case MsgType::Register: return "REGISTER";
    case MsgType::RegisterAck: return "REGISTER_ACK";
    case MsgType::ConnectReq: return "CONNECT_REQ";
    case MsgType::ConnectRsp: return "CONNECT_RSP";
    case MsgType::Ping: return "PING";
    case MsgType::Pong: return "PONG";
    case MsgType::Heartbeat: return "HEARTBEAT";
    case MsgType::SessionInvite: return "SESSION_INVITE";
    case MsgType::SessionAccept: return "SESSION_ACCEPT";
    case MsgType::SessionReject: return "SESSION_REJECT";
    case MsgType::AuthChallenge: return "AUTH_CHALLENGE";
    case MsgType::AuthResponse: return "AUTH_RESPONSE";
    case MsgType::AuthResult: return "AUTH_RESULT";
    case MsgType::SessionClose: return "SESSION_CLOSE";
    case MsgType::CapsNegotiate: return "CAPS_NEGOTIATE";
    case MsgType::InputMouseMove: return "INPUT_MOUSE_MOVE";
    case MsgType::InputMouseButton: return "INPUT_MOUSE_BUTTON";
    case MsgType::InputMouseWheel: return "INPUT_MOUSE_WHEEL";
    case MsgType::InputKey: return "INPUT_KEY";
    case MsgType::InputText: return "INPUT_TEXT";
    case MsgType::InputGesture: return "INPUT_GESTURE";
    case MsgType::VideoConfig: return "VIDEO_CONFIG";
    case MsgType::VideoFrame: return "VIDEO_FRAME";
    case MsgType::KeyframeReq: return "KEYFRAME_REQ";
    case MsgType::VideoStats: return "VIDEO_STATS";
    case MsgType::ClipboardSet: return "CLIPBOARD_SET";
    case MsgType::ClipboardData: return "CLIPBOARD_DATA";
    case MsgType::FileOffer: return "FILE_OFFER";
    case MsgType::FileAccept: return "FILE_ACCEPT";
    case MsgType::FileChunk: return "FILE_CHUNK";
    case MsgType::FileEnd: return "FILE_END";
    case MsgType::FileAbort: return "FILE_ABORT";
    case MsgType::Error: return "ERROR";
  }
  return "UNKNOWN";
}

}  // namespace rc
