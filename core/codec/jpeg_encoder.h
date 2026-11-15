#pragma once

#ifdef _WIN32

#include <windows.h>
#include <objidl.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <string>
#include <vector>

#include "codec/encoder.h"

namespace rc {

// WIC JPEG 编码，全程内存态（HGLOBAL + IStream），不落盘。
class JpegEncoder : public IVideoEncoder {
 public:
  bool open(int width, int height, int fps, int quality, std::string* error) override;
  void close() override;
  bool encode(const CapturedFrame& source, EncodedFrame& out,
              std::string* error) override;
  // JPEG 每帧都是独立帧，任何一帧都能直接解码，无需额外生成关键帧。
  bool request_keyframe() override { return true; }
  const char* name() const override { return "jpeg-wic"; }

 private:
  Microsoft::WRL::ComPtr<IWICImagingFactory> factory_;
  std::vector<uint8_t> bgr_buffer_;
  int width_ = 0;
  int height_ = 0;
  int quality_ = 75;
};

}  // namespace rc

#endif  // _WIN32
