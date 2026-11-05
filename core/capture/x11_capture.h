#pragma once

#ifdef __linux__

#include <X11/Xlib.h>

#include <cstdint>
#include <string>

#include "capture/capture.h"

namespace rc {

class X11Capture : public ICaptureSource {
 public:
  X11Capture();
  ~X11Capture() override;

  bool start(int monitor_index, std::string* error) override;
  void stop() override;
  bool grab(CapturedFrame& frame, int timeout_ms) override;
  bool set_output_format(PixelFormat format) override;
  const char* name() const override { return "x11"; }

 private:
  Display* display_ = nullptr;
  Window root_ = 0;
  int screen_ = 0;
  int width_ = 0;
  int height_ = 0;

  bool use_xshm_ = false;
  void* shm_info_ = nullptr;
  XImage* shm_img_ = nullptr;
  unsigned char* shm_data_ = nullptr;

  int depth_ = 0;
  int bits_per_pixel_ = 0;
  uint32_t red_mask_ = 0;
  uint32_t green_mask_ = 0;
  uint32_t blue_mask_ = 0;

  PixelFormat desired_format_ = PixelFormat::kBGRA;
  bool started_ = false;
  // grab() 按 timeout_ms 节流：X11 没有“等新画面”的事件接口，
  // 不节流就会空转抓同一帧，白烧一整核 CPU。
  uint64_t last_grab_us_ = 0;
};

}  // namespace rc

#endif  // __linux__
