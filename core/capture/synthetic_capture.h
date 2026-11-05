#pragma once

#include <chrono>
#include <thread>

#include "capture/capture.h"
#include "base/clock.h"

namespace rc {

// 无显示器/无桌面权限时的兜底采集：生成会移动的彩色条纹，用于联调链路。
class SyntheticCapture : public ICaptureSource {
 public:
  bool start(int monitor_index, std::string* error) override {
    (void)monitor_index;
    (void)error;
    phase_ = 0;
    return true;
  }

  void stop() override {}

  bool grab(CapturedFrame& frame, int timeout_ms) override {
    const int wait = timeout_ms <= 0 ? 0 : (timeout_ms < 33 ? timeout_ms : 33);
    if (wait > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(wait));
    }

    constexpr int kWidth = 1280;
    constexpr int kHeight = 720;

    frame.width = kWidth;
    frame.height = kHeight;
    frame.stride = kWidth * 4;
    frame.capture_ts_us = static_cast<int64_t>(now_us());
    frame.pixels.resize(static_cast<size_t>(kWidth) * kHeight * 4);

    for (int y = 0; y < kHeight; ++y) {
      uint8_t* row = frame.pixels.data() + static_cast<size_t>(y) * frame.stride;
      for (int x = 0; x < kWidth; ++x) {
        const uint8_t b = static_cast<uint8_t>((x + phase_) & 0xFF);
        const uint8_t g = static_cast<uint8_t>(y & 0xFF);
        const uint8_t r = static_cast<uint8_t>(((x + y + phase_) >> 1) & 0xFF);
        row[x * 4 + 0] = b;
        row[x * 4 + 1] = g;
        row[x * 4 + 2] = r;
        row[x * 4 + 3] = 0xFF;
      }
    }

    phase_ += 4;
    return true;
  }

  const char* name() const override { return "synthetic"; }

 private:
  int phase_ = 0;
};

}  // namespace rc
