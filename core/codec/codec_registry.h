#pragma once

#include <string>
#include <vector>

namespace rc {

// 本端某个 codec 的实际可用性：不是"是否编译进来"，而是真正打开编码器/解码器成功。
struct CodecAvailability {
  bool encode = false;
  bool encode_hardware = false;
  bool decode = false;
  bool decode_hardware = false;
};

// 探测结果按进程缓存，首次调用会实际创建并打开编解码器。
CodecAvailability codec_availability(const std::string& codec);

// 按偏好顺序返回本端实际可用的 codec（vp9 优先，h264 兜底）。
std::vector<std::string> encodable_codecs();
std::vector<std::string> decodable_codecs();

}  // namespace rc
