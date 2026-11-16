#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rc {

struct DecodedImage {
  int width = 0;
  int height = 0;
  int stride = 0;
  int coded_height = 0;
  bool nv12 = false;
  uint64_t timestamp_us = 0;
  std::vector<uint8_t> pixels;
};

// 解码器抽象接口。Windows（Media Foundation MFT）与 Linux（FFmpeg）各有实现，
// 由 create_decoder() 按平台分发，调用方无需关心具体后端。
class IVideoDecoder {
 public:
  virtual ~IVideoDecoder() = default;

  // 输出格式：BGRA（CPU 像素，可直接编码/落盘）或 NV12（原始平面，交给 GPU 上屏）。
  enum class Output { kBgra, kNv12 };
  virtual void set_output(Output output) = 0;

  virtual bool open(std::string* error) = 0;
  virtual void close() = 0;
  virtual const char* name() const = 0;

  // 输入一帧完整编码帧（H.264 为 Annex-B，VP9 为原始压缩帧）；
  // 只有真正解出一幅画面时才返回 true。
  virtual bool decode(const uint8_t* data, std::size_t len, int64_t timestamp_us,
                      DecodedImage& out, std::string* error) = 0;
  // 实际使用的后端是否为硬件解码（用于能力上报与前端展示）。
  virtual bool is_hardware() const { return false; }
};

// 按协商出的 codec（"h264" / "vp9"）创建解码器。
std::unique_ptr<IVideoDecoder> create_decoder(const std::string& codec,
                                              std::string* error);

}  // namespace rc
