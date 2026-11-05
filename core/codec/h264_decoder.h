#pragma once

#ifdef _WIN32

#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "codec/decoder.h"

namespace rc {

class VideoProcessorConverter;

// Media Foundation H.264 解码（同步 MFT）。
// 优先让解码器直接输出 RGB32（内部走颜色转换 DSP），省掉 CPU 的 NV12→BGRA。
class H264Decoder : public IVideoDecoder {
 public:
  H264Decoder();
  ~H264Decoder() override;

  void set_output(Output output) override { want_nv12_ = output == Output::kNv12; }

  bool open(std::string* error) override;
  void close() override;
  const char* name() const override {
    return output_rgb32_ ? "h264-mft-rgb32" : "h264-mft-nv12";
  }

  // 输入一帧 Annex-B 码流；只有真正解出一幅画面时才返回 true。
  bool decode(const uint8_t* data, std::size_t len, int64_t timestamp_us,
              DecodedImage& out, std::string* error) override;

 private:
  bool pick_output_type(std::string* error);
  bool pull_output(DecodedImage& out, std::string* error);

  Microsoft::WRL::ComPtr<IMFTransform> transform_;
  bool com_initialized_ = false;
  bool mf_started_ = false;
  bool output_rgb32_ = false;
  bool provides_samples_ = false;
  uint32_t output_buffer_size_ = 0;
  int width_ = 0;
  int height_ = 0;
  int visible_height_ = 0;
  int output_stride_ = 0;
  bool converter_failed_ = false;
  bool want_nv12_ = false;
  // 诊断：解码（ProcessOutput）与色彩转换的耗时拆分。
  uint64_t stat_decode_us_ = 0;
  uint64_t stat_convert_us_ = 0;
  uint64_t stat_frames_ = 0;
  // NV12 -> BGRA 的 GPU 转换；不可用时回退到 CPU 转换。
  std::unique_ptr<VideoProcessorConverter> converter_;
  // 解码器缓冲导致的“已解出但没来得及交出去”的画面，避免丢帧。
  std::deque<DecodedImage> ready_;
  // 复用的输入样本：避免每帧 MFCreateMemoryBuffer/MFCreateSample。
  Microsoft::WRL::ComPtr<IMFSample> input_sample_;
  Microsoft::WRL::ComPtr<IMFMediaBuffer> input_buffer_;
  uint32_t input_buffer_capacity_ = 0;
};

}  // namespace rc

#endif  // _WIN32
