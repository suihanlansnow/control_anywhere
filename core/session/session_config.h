#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace rc {

struct SessionConfig {
  std::string relay_host = "127.0.0.1";
  uint16_t relay_port = 21116;
  std::string device_id;
  std::string alias;
  std::string target_id;
  std::string password;
  std::string codec = "h264";
  std::string capture;
  int fps = 60;
  int quality = 75;
  int monitor = 0;
  int max_frames = 0;
  bool render = true;
};

using LogCallback = std::function<void(const std::string& tag, const std::string& level, const std::string& msg)>;
using StatusCallback = std::function<void(const std::string& state)>;
// 字节数用 64 位：长时间会话累计会超过 2GB，int 会溢出成负数。
using ProgressCallback = std::function<void(int64_t sent, int64_t bytes_total)>;

}  // namespace rc
