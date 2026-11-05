#include "net/connection.h"

#include <cstring>

#include "base/log.h"

namespace rc {

bool Connection::build_frame_header(uint8_t* out, MsgType type, uint64_t session_id,
                                    uint32_t seq, uint8_t flags,
                                    std::size_t payload_len) {
  if (payload_len > kMaxFramePayload) {
    RC_LOGE("conn", "payload exceeds limit: " << payload_len);
    return false;
  }
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
  return true;
}

bool Connection::send(MsgType type, uint64_t session_id, const uint8_t* payload,
                      std::size_t length, uint32_t seq, uint8_t flags) {
  std::lock_guard<std::mutex> lock(send_mutex_);
  uint8_t header[kFrameHeaderSize];
  if (!build_frame_header(header, type, session_id, seq, flags, length)) return false;
  if (payload == nullptr || length == 0) {
    return socket_.send_all(header, kFrameHeaderSize);
  }
  // 帧头与载荷分散写：不再把整帧拷进临时缓冲（中继转发路径最受益）。
  const uint8_t* buffers[2] = {header, payload};
  const std::size_t sizes[2] = {kFrameHeaderSize, length};
  return socket_.send_vectored(buffers, sizes, 2);
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
  uint8_t frame_header[kFrameHeaderSize];
  if (!build_frame_header(frame_header, MsgType::VideoFrame, session_id, seq, 0,
                          video_payload_len)) {
    return false;
  }

  // 视频头（32 字节）写在栈缓冲上，随后与码流一起分散写。
  uint8_t video_header[kVideoHeaderBytes] = {};
  video_header[0] = static_cast<uint8_t>(meta.capture_ts_us & 0xFF);
  video_header[1] = static_cast<uint8_t>((meta.capture_ts_us >> 8) & 0xFF);
  video_header[2] = static_cast<uint8_t>((meta.capture_ts_us >> 16) & 0xFF);
  video_header[3] = static_cast<uint8_t>((meta.capture_ts_us >> 24) & 0xFF);
  video_header[4] = static_cast<uint8_t>((meta.capture_ts_us >> 32) & 0xFF);
  video_header[5] = static_cast<uint8_t>((meta.capture_ts_us >> 40) & 0xFF);
  video_header[6] = static_cast<uint8_t>((meta.capture_ts_us >> 48) & 0xFF);
  video_header[7] = static_cast<uint8_t>((meta.capture_ts_us >> 56) & 0xFF);
  video_header[8] = static_cast<uint8_t>(meta.encode_ts_us & 0xFF);
  video_header[9] = static_cast<uint8_t>((meta.encode_ts_us >> 8) & 0xFF);
  video_header[10] = static_cast<uint8_t>((meta.encode_ts_us >> 16) & 0xFF);
  video_header[11] = static_cast<uint8_t>((meta.encode_ts_us >> 24) & 0xFF);
  video_header[12] = static_cast<uint8_t>((meta.encode_ts_us >> 32) & 0xFF);
  video_header[13] = static_cast<uint8_t>((meta.encode_ts_us >> 40) & 0xFF);
  video_header[14] = static_cast<uint8_t>((meta.encode_ts_us >> 48) & 0xFF);
  video_header[15] = static_cast<uint8_t>((meta.encode_ts_us >> 56) & 0xFF);
  video_header[16] = static_cast<uint8_t>(meta.frame_id & 0xFF);
  video_header[17] = static_cast<uint8_t>((meta.frame_id >> 8) & 0xFF);
  video_header[18] = static_cast<uint8_t>((meta.frame_id >> 16) & 0xFF);
  video_header[19] = static_cast<uint8_t>((meta.frame_id >> 24) & 0xFF);
  video_header[20] = static_cast<uint8_t>(meta.width & 0xFF);
  video_header[21] = static_cast<uint8_t>((meta.width >> 8) & 0xFF);
  video_header[22] = static_cast<uint8_t>(meta.height & 0xFF);
  video_header[23] = static_cast<uint8_t>((meta.height >> 8) & 0xFF);
  video_header[24] = meta.codec;
  video_header[25] = meta.keyframe ? 0x01 : 0x00;
  video_header[26] = 0;
  video_header[27] = 0;
  const uint32_t dlen = static_cast<uint32_t>(length);
  video_header[28] = static_cast<uint8_t>(dlen & 0xFF);
  video_header[29] = static_cast<uint8_t>((dlen >> 8) & 0xFF);
  video_header[30] = static_cast<uint8_t>((dlen >> 16) & 0xFF);
  video_header[31] = static_cast<uint8_t>((dlen >> 24) & 0xFF);

  const uint8_t* buffers[3] = {frame_header, video_header, data};
  const std::size_t sizes[3] = {kFrameHeaderSize, kVideoHeaderBytes, length};
  const int count = (data != nullptr && length > 0) ? 3 : 2;
  return socket_.send_vectored(buffers, sizes, count);
}

bool Connection::recv(DecodedFrame& out, bool* timed_out) {
  uint8_t raw_header[kFrameHeaderSize];
  bool header_timeout = false;
  if (!socket_.recv_exact(raw_header, kFrameHeaderSize, &header_timeout)) {
    if (timed_out) *timed_out = header_timeout;
    return false;
  }

  FrameHeader header;
  if (!decode_frame_header(raw_header, kFrameHeaderSize, header)) {
    if (timed_out) *timed_out = false;
    return false;
  }

  out.type = static_cast<MsgType>(header.type);
  out.flags = header.flags;
  out.msg_id = header.msg_id;
  out.session_id = header.session_id;
  out.seq = header.seq;
  out.payload.resize(header.payload_len);

  if (header.payload_len > 0) {
    bool payload_timeout = false;
    if (!socket_.recv_exact(out.payload.data(), header.payload_len, &payload_timeout)) {
      if (timed_out) *timed_out = payload_timeout;
      return false;
    }
  }
  if (timed_out) *timed_out = false;
  return true;
}

}  // namespace rc
