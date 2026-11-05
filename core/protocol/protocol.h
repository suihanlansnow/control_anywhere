#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rc {

constexpr uint8_t kProtocolVersion = 1;
constexpr std::size_t kFrameHeaderSize = 24;
constexpr uint32_t kMaxFramePayload = 1u << 20;  // 1 MiB
constexpr uint16_t kDefaultControlPort = 21116;
constexpr uint16_t kDefaultMediaPort = 21117;

enum class MsgType : uint8_t {
  HelloReq = 0x01,
  HelloRsp = 0x02,
  Register = 0x03,
  RegisterAck = 0x04,
  ConnectReq = 0x05,
  ConnectRsp = 0x06,
  Ping = 0x08,
  Pong = 0x09,

  SessionInvite = 0x10,
  SessionAccept = 0x11,
  SessionReject = 0x12,
  AuthChallenge = 0x13,
  AuthResponse = 0x14,
  AuthResult = 0x15,
  SessionClose = 0x16,
  CapsNegotiate = 0x18,

  InputMouseMove = 0x20,
  InputMouseButton = 0x21,
  InputMouseWheel = 0x22,
  InputKey = 0x23,
  InputText = 0x24,
  InputGesture = 0x26,

  VideoConfig = 0x30,
  VideoFrame = 0x31,
  KeyframeReq = 0x32,
  VideoStats = 0x33,

  ClipboardSet = 0x50,
  ClipboardData = 0x51,
  FileOffer = 0x52,
  FileAccept = 0x53,
  FileChunk = 0x54,
  FileEnd = 0x55,
  FileAbort = 0x56,

  Error = 0x61,
};

enum FrameFlag : uint8_t {
  FlagEncrypted = 1u << 0,
  FlagMoreFragment = 1u << 1,
  FlagLastFragment = 1u << 2,
  FlagCompressed = 1u << 3,
};

struct FrameHeader {
  uint8_t version = kProtocolVersion;
  uint8_t type = 0;
  uint8_t flags = 0;
  uint8_t reserved = 0;
  uint32_t msg_id = 0;
  uint64_t session_id = 0;
  uint32_t seq = 0;
  uint32_t payload_len = 0;
};

// 24 字节头，小端序，字段自然对齐。
std::vector<uint8_t> encode_frame(const FrameHeader& header, const uint8_t* payload,
                                  std::size_t payload_len);

// 校验版本号与长度上限；失败返回 false。
bool decode_frame_header(const uint8_t* data, std::size_t available, FrameHeader& out);

const char* msg_type_name(MsgType type);

}  // namespace rc
