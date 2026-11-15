#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "capture/capture.h"

namespace rc {

// codec 取值与 VIDEO_FRAME 协议字段一致。
enum class VideoCodec : uint8_t { H264 = 1, H265 = 2, AV1 = 3, Jpeg = 4 };

struct EncodedFrame {
  VideoCodec codec = VideoCodec::Jpeg;
  bool keyframe = false;
  int width = 0;
  int height = 0;
  uint64_t capture_ts_us = 0;
  uint64_t encode_ts_us = 0;
  std::vector<uint8_t> data;
};

class IVideoEncoder {
 public:
  virtual ~IVideoEncoder() = default;

  virtual bool open(int width, int height, int fps, int quality, std::string* error) = 0;
  virtual void close() = 0;
  virtual bool encode(const CapturedFrame& source, EncodedFrame& out,
                      std::string* error) = 0;
  // 请求下一帧编码为关键帧（IDR）。编码器不支持时返回 false。
  virtual bool request_keyframe() { return false; }
  // 目标码率（kbps），0 表示编码器未使用固定码率；用于 VIDEO_CONFIG 下发。
  virtual int target_bitrate_kbps() const { return 0; }
  // 可选：共享采集侧 D3D11 设备（不透明指针），硬件 MFT 用它做 GPU 纹理零拷贝。
  // 必须在 open() 之前调用。
  virtual bool set_d3d_device(void* device) { return false; }
  // open() 之后查询：是否可以直接接收 CapturedFrame::gpu_texture。
  virtual bool accepts_texture_input() const { return false; }
  // open() 之后可用：MFT 使用的 MF 设备管理器，采集端据此串行化 GPU 访问。
  virtual void* native_device_manager() { return nullptr; }
  virtual const char* name() const = 0;
};

// kind: "jpeg"（Windows WIC）
std::unique_ptr<IVideoEncoder> create_encoder(const std::string& kind,
                                              std::string* error);

const char* codec_name(VideoCodec codec);

}  // namespace rc
