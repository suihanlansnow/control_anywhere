#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rc {

constexpr std::size_t kVideoHeaderBytes = 32;

struct VideoFrameMeta {
  uint64_t capture_ts_us = 0;
  uint64_t encode_ts_us = 0;
  uint32_t frame_id = 0;
  uint16_t width = 0;
  uint16_t height = 0;
  uint8_t codec = 4;  // 1=h264 2=h265 3=av1 4=jpeg
  bool keyframe = false;
  uint32_t data_len = 0;
};

// VIDEO_FRAME 载荷：32 字节头 + 编码码流，与 proto/messages.yaml 保持一致。
std::vector<uint8_t> build_video_payload(const VideoFrameMeta& meta, const uint8_t* data,
                                         std::size_t length);

bool parse_video_payload(const std::vector<uint8_t>& payload, VideoFrameMeta& meta);

}  // namespace rc
