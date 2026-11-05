#include "net/connection.h"

#include <cstring>

#include "base/log.h"

namespace rc {

uint8_t* Connection::write_frame_header(MsgType type, uint64_t session_id,
                                        uint32_t seq, uint8_t flags,
                                        std::size_t payload_len) {
  if (payload_len > kMaxFramePayload) {
    RC_LOGE("conn", "payload exceeds limit: " << payload_len);
    return nullptr;
  }
  send_buf_.resize(kFrameHeaderSize + payload_len);
  uint8_t* out = send_buf_.data();
  out[0] = kProtocolVersion;
  out[1] = static_cast<uint8_t>(type);
  out[2] = flags;
  out[3] = 0;  // reserved
  // msg_id (u32 LE) at offset 4
  const uint32_t msg_id = next_msg_id();
  out[4] = static_cast<uint8_t>(msg_id & 0xFF);
  out[5] = static_cast<uint8_t>((msg_id >> 8) & 0xFF);
  out[6] = static_cast<uint8_t>((msg_id >> 16) & 0xFF);
  out[7] = static_cast<uint8_t>((msg_id >> 24) & 0xFF);
  // session_id (u64 LE) at offset 8
  for (int i = 0; i < 8; ++i) {
    out[8 + i] = static_cast<uint8_t>((session_id >> (i * 8)) & 0xFF);
  }
  // seq (u32 LE) at offset 16
  out[16] = static_cast<uint8_t>(seq & 0xFF);
  out[17] = static_cast<uint8_t>((seq >> 8) & 0xFF);
  out[18] = static_cast<uint8_t>((seq >> 16) & 0xFF);
  out[19] = static_cast<uint8_t>((seq >> 24) & 0xFF);
  // payload_len (u32 LE) at offset 20
  const uint32_t plen = static_cast<uint32_t>(payload_len);
  out[20] = static_cast<uint8_t>(plen & 0xFF);
  out[21] = static_cast<uint8_t>((plen >> 8) & 0xFF);
  out[22] = static_cast<uint8_t>((plen >> 16) & 0xFF);
  out[23] = static_cast<uint8_t>((plen >> 24) & 0xFF);
  return out + kFrameHeaderSize;
}

bool Connection::send(MsgType type, uint64_t session_id, const uint8_t* payload,
                      std::size_t length, uint32_t seq, uint8_t flags) {
  std::lock_guard<std::mutex> lock(send_mutex_);
  uint8_t* dst = write_frame_header(type, session_id, seq, flags, length);
  if (dst == nullptr) return false;
  if (length > 0 && payload != nullptr) {
    std::memcpy(dst, payload, length);
  }
  return socket_.send_all(send_buf_.data(), send_buf_.size());
}

bool Connection::send_json(MsgType type, uint64_t session_id,
                           const json::Value& value, uint32_t seq, uint8_t flags) {
  const std::string text = value.dump();
  return send(type, session_id, reinterpret_cast<const uint8_t*>(text.data()),
              text.size(), seq, flags);
}

bool Connection::send_video_frame(uint64_t session_id, const VideoFrameMeta& meta,
                                  const uint8_t* data, std::size_t length,
                                  uint32_t seq) {
  std::lock_guard<std::mutex> lock(send_mutex_);
  const std::size_t video_payload_len = kVideoHeaderBytes + length;
  uint8_t* payload = write_frame_header(MsgType::VideoFrame, session_id, seq, 0,
                                        video_payload_len);
  if (payload == nullptr) return false;

  // 视频头（28 字节）直接写进 payload 区。
  payload[0] = static_cast<uint8_t>(meta.capture_ts_us & 0xFF);
  payload[1] = static_cast<uint8_t>((meta.capture_ts_us >> 8) & 0xFF);
  payload[2] = static_cast<uint8_t>((meta.capture_ts_us >> 16) & 0xFF);
  payload[3] = static_cast<uint8_t>((meta.capture_ts_us >> 24) & 0xFF);
  payload[4] = static_cast<uint8_t>((meta.capture_ts_us >> 32) & 0xFF);
  payload[5] = static_cast<uint8_t>((meta.capture_ts_us >> 40) & 0xFF);
  payload[6] = static_cast<uint8_t>((meta.capture_ts_us >> 48) & 0xFF);
  payload[7] = static_cast<uint8_t>((meta.capture_ts_us >> 56) & 0xFF);
  payload[8] = static_cast<uint8_t>(meta.encode_ts_us & 0xFF);
  payload[9] = static_cast<uint8_t>((meta.encode_ts_us >> 8) & 0xFF);
  payload[10] = static_cast<uint8_t>((meta.encode_ts_us >> 16) & 0xFF);
  payload[11] = static_cast<uint8_t>((meta.encode_ts_us >> 24) & 0xFF);
  payload[12] = static_cast<uint8_t>((meta.encode_ts_us >> 32) & 0xFF);
  payload[13] = static_cast<uint8_t>((meta.encode_ts_us >> 40) & 0xFF);
  payload[14] = static_cast<uint8_t>((meta.encode_ts_us >> 48) & 0xFF);
  payload[15] = static_cast<uint8_t>((meta.encode_ts_us >> 56) & 0xFF);
  payload[16] = static_cast<uint8_t>(meta.frame_id & 0xFF);
  payload[17] = static_cast<uint8_t>((meta.frame_id >> 8) & 0xFF);
  payload[18] = static_cast<uint8_t>((meta.frame_id >> 16) & 0xFF);
  payload[19] = static_cast<uint8_t>((meta.frame_id >> 24) & 0xFF);
  payload[20] = static_cast<uint8_t>(meta.width & 0xFF);
  payload[21] = static_cast<uint8_t>((meta.width >> 8) & 0xFF);
  payload[22] = static_cast<uint8_t>(meta.height & 0xFF);
  payload[23] = static_cast<uint8_t>((meta.height >> 8) & 0xFF);
  payload[24] = meta.codec;
  payload[25] = meta.keyframe ? 0x01 : 0x00;
  payload[26] = 0;
  payload[27] = 0;
  const uint32_t dlen = static_cast<uint32_t>(length);
  payload[28] = static_cast<uint8_t>(dlen & 0xFF);
  payload[29] = static_cast<uint8_t>((dlen >> 8) & 0xFF);
  payload[30] = static_cast<uint8_t>((dlen >> 16) & 0xFF);
  payload[31] = static_cast<uint8_t>((dlen >> 24) & 0xFF);

  if (length > 0 && data != nullptr) {
    std::memcpy(payload + kVideoHeaderBytes, data, length);
  }
  return socket_.send_all(send_buf_.data(), send_buf_.size());
}

bool Connection::recv(DecodedFrame& out) {
  uint8_t raw_header[kFrameHeaderSize];
  if (!socket_.recv_exact(raw_header, kFrameHeaderSize)) return false;

  FrameHeader header;
  if (!decode_frame_header(raw_header, kFrameHeaderSize, header)) return false;

  out.type = static_cast<MsgType>(header.type);
  out.flags = header.flags;
  out.msg_id = header.msg_id;
  out.session_id = header.session_id;
  out.seq = header.seq;
  out.payload.resize(header.payload_len);

  if (header.payload_len > 0 &&
      !socket_.recv_exact(out.payload.data(), header.payload_len)) {
    return false;
  }
  return true;
}

}  // namespace rc
