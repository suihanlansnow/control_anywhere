#include "protocol/video_payload.h"

#include <cstring>

namespace rc {
namespace {

void put_u16(std::vector<uint8_t>& buffer, std::size_t offset, uint16_t value) {
  buffer[offset + 0] = static_cast<uint8_t>(value & 0xFF);
  buffer[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
}

void put_u32(std::vector<uint8_t>& buffer, std::size_t offset, uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    buffer[offset + i] = static_cast<uint8_t>((value >> (i * 8)) & 0xFF);
  }
}

void put_u64(std::vector<uint8_t>& buffer, std::size_t offset, uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    buffer[offset + i] = static_cast<uint8_t>((value >> (i * 8)) & 0xFF);
  }
}

uint16_t get_u16(const uint8_t* data) {
  return static_cast<uint16_t>(data[0] | (data[1] << 8));
}

uint32_t get_u32(const uint8_t* data) {
  uint32_t value = 0;
  for (int i = 0; i < 4; ++i) value |= static_cast<uint32_t>(data[i]) << (i * 8);
  return value;
}

uint64_t get_u64(const uint8_t* data) {
  uint64_t value = 0;
  for (int i = 0; i < 8; ++i) value |= static_cast<uint64_t>(data[i]) << (i * 8);
  return value;
}

}  // namespace

std::vector<uint8_t> build_video_payload(const VideoFrameMeta& meta, const uint8_t* data,
                                         std::size_t length) {
  std::vector<uint8_t> payload(kVideoHeaderBytes + length, 0);
  put_u64(payload, 0, meta.capture_ts_us);
  put_u64(payload, 8, meta.encode_ts_us);
  put_u32(payload, 16, meta.frame_id);
  put_u16(payload, 20, meta.width);
  put_u16(payload, 22, meta.height);
  payload[24] = meta.codec;
  payload[25] = meta.keyframe ? 0x01 : 0x00;
  payload[26] = 0;
  payload[27] = 0;
  put_u32(payload, 28, static_cast<uint32_t>(length));
  if (length > 0 && data != nullptr) {
    std::memcpy(payload.data() + kVideoHeaderBytes, data, length);
  }
  return payload;
}

bool parse_video_payload(const std::vector<uint8_t>& payload, VideoFrameMeta& meta) {
  if (payload.size() < kVideoHeaderBytes) return false;
  const uint8_t* data = payload.data();
  meta.capture_ts_us = get_u64(data + 0);
  meta.encode_ts_us = get_u64(data + 8);
  meta.frame_id = get_u32(data + 16);
  meta.width = get_u16(data + 20);
  meta.height = get_u16(data + 22);
  meta.codec = data[24];
  meta.keyframe = (data[25] & 0x01) != 0;
  meta.data_len = get_u32(data + 28);
  return payload.size() >= kVideoHeaderBytes + meta.data_len;
}

}  // namespace rc
