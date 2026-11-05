#pragma once

#ifdef _WIN32

#include <d3d11.h>
#include <dxgi1_2.h>
#include <mfidl.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "capture/capture.h"

namespace rc {

// Windows Desktop Duplication（DXGI 1.2）抓屏。
// 首次调用 grab 时初始化；桌面会话不可用时 start 返回 false，由调用方降级到 synthetic。
// 请求 NV12 时用 D3D11 VideoProcessor 在 GPU 上做 BGRA->NV12；
// 打开纹理直通后只交出 GPU 纹理（零回读），否则回读 3MB NV12 平面。
class DxgiCapture : public ICaptureSource {
 public:
  bool start(int monitor_index, std::string* error) override;
  void stop() override;
  bool grab(CapturedFrame& frame, int timeout_ms) override;
  bool set_output_format(PixelFormat format) override;
  bool set_texture_output(bool enabled) override;
  bool set_device_manager(void* manager) override;
  void* native_device() override { return device_.Get(); }
  const char* name() const override { return "dxgi"; }

 private:
  // 采集侧的 GPU 调用必须和 MFT 内部的 GPU 工作串行，共用 MF 设备管理器的锁。
  class DeviceLockGuard {
   public:
    explicit DeviceLockGuard(DxgiCapture& owner) : owner_(owner) { owner_.lock_device(); }
    ~DeviceLockGuard() { owner_.unlock_device(); }

   private:
    DxgiCapture& owner_;
  };

  void lock_device();
  void unlock_device();

  // 一帧 NV12 输出纹理。handle 分发给 CapturedFrame，use_count 用来判断是否还在被编码器使用。
  struct OutputSlot {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> view;
    std::shared_ptr<void> handle;
  };

  bool create_duplication(std::string* error);
  bool ensure_staging(int width, int height, DXGI_FORMAT format);
  bool ensure_convert(int width, int height, std::string* error);
  void release_convert();
  bool blit_nv12(ID3D11VideoProcessorOutputView* view);
  bool grab_nv12_texture(CapturedFrame& frame, std::string* error);
  bool grab_nv12_cpu(CapturedFrame& frame, std::string* error);

  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
  Microsoft::WRL::ComPtr<IDXGIOutputDuplication> duplication_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_;

  // ---- BGRA -> NV12 的 GPU 转换链路 ----
  Microsoft::WRL::ComPtr<ID3D11VideoDevice> video_device_;
  Microsoft::WRL::ComPtr<ID3D11VideoContext> video_context_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> copied_texture_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> nv12_texture_;
  // 双缓冲 staging + 完成查询：本帧写入 staging[write]，读上一帧 staging[read]，
  // 让 GPU 回读与下一帧采集重叠，消除 Map 阻塞。
  Microsoft::WRL::ComPtr<ID3D11Texture2D> nv12_staging_[2];
  Microsoft::WRL::ComPtr<ID3D11Query> gpu_done_query_[2];
  int staging_write_idx_ = 0;
  bool staging_pipeline_primed_ = false;
  Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> enumerator_;
  Microsoft::WRL::ComPtr<ID3D11VideoProcessor> processor_;
  Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> input_view_;
  Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> cpu_output_view_;
  std::vector<OutputSlot> slots_;
  bool convert_failed_ = false;
  int convert_width_ = 0;
  int convert_height_ = 0;

  // 诊断计数：acquire / gpu 转换 / 回读各阶段耗时。
  uint64_t stat_frames_ = 0;
  uint64_t stat_acquire_us_ = 0;
  uint64_t stat_convert_us_ = 0;
  uint64_t stat_readback_us_ = 0;

  PixelFormat format_ = PixelFormat::kBGRA;
  std::atomic<bool> texture_output_{false};
  // MF 设备管理器：OpenDeviceHandle 拿到的句柄用于 LockDevice/UnlockDevice。
  Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> device_manager_;
  HANDLE device_handle_ = nullptr;
  bool device_locked_ = false;
  int monitor_index_ = 0;
  int width_ = 0;
  int height_ = 0;
  DXGI_FORMAT staging_format_ = DXGI_FORMAT_UNKNOWN;
};

}  // namespace rc

#endif  // _WIN32
