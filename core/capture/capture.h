#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rc {

// 采集输出的像素格式。
// BGRA：CPU 侧像素（JPEG 编码、无 GPU 兜底用）。
// NV12：GPU 直接转成编码器要的格式，省掉 8MB 回读 + CPU 逐像素转换。
enum class PixelFormat : uint8_t { kBGRA = 0, kNV12 = 1 };

// 统一采集输出格式。BGRA 时 stride 为行字节数（可能因显存对齐大于 width*4）；
// NV12 时 stride 为亮度平面行宽（== width），pixels 为紧凑 Y 平面 + UV 平面。
struct CapturedFrame {
  int width = 0;
  int height = 0;
  int stride = 0;
  PixelFormat format = PixelFormat::kBGRA;
  int64_t capture_ts_us = 0;
  std::vector<uint8_t> pixels;
  // 可选：GPU 上的 NV12 纹理（不透明句柄，实际类型是 ID3D11Texture2D）。
  // 非空时 pixels 为空，编码器可以零拷贝直接把它送进 MFT。
  std::shared_ptr<void> gpu_texture;
};

class ICaptureSource {
 public:
  virtual ~ICaptureSource() = default;

  virtual bool start(int monitor_index, std::string* error) = 0;
  virtual void stop() = 0;

  // 无新帧时返回 false（实现了超时等待，调用方可以直接循环）。
  virtual bool grab(CapturedFrame& frame, int timeout_ms) = 0;

  // 请求输出格式；不支持时返回 false，调用方继续按 BGRA 处理。
  virtual bool set_output_format(PixelFormat format) {
    return format == PixelFormat::kBGRA;
  }

  // 是否改为交付 GPU 纹理（零回读）。消费端不吃纹理时必须保持 false。
  virtual bool set_texture_output(bool enabled) { return false; }

  // 内部 D3D11 设备（不透明指针），供编码器共享同一设备做零拷贝。
  virtual void* native_device() { return nullptr; }

  // 共享编码器创建的 MF 设备管理器：采集侧的 GPU 调用要和 MFT 内的 GPU 工作
  // 串行执行，否则同一块 D3D11 设备会被两个线程同时使用而卡死。
  virtual bool set_device_manager(void* manager) { return false; }

  virtual const char* name() const = 0;
};

// kind: "dxgi"（仅 Windows）/"synthetic"（全平台，联调兜底）
std::unique_ptr<ICaptureSource> create_capture_source(const std::string& kind,
                                                      std::string* error);

}  // namespace rc
