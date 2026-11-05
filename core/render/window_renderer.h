#pragma once

#ifdef _WIN32

#include <windows.h>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>

#include "render/renderer.h"

namespace rc {

// Win32 窗口 + 显示。
// 有可用 GPU 时用 DXGI swapchain：NV12 交给 VideoProcessor 直接画进 back buffer，
// 全程不回读，颜色转换与缩放都在 GPU 上做；否则退回 GDI StretchDIBits（需要 BGRA）。
class WindowRenderer : public IRenderer {
 public:
  ~WindowRenderer() override;

  bool open(const std::string& title, int width, int height,
            std::string* error) override;
  void close() override;
  bool alive() const override { return alive_; }

  // true 表示 swapchain 已就绪，可以吃 NV12 直出。
  bool gpu() const override { return swapchain_ != nullptr; }

  // 非阻塞处理窗口消息（拖拽/关闭），需在主循环里持续调用。
  void pump() override;

  // NV12 直出：coded_height 为编码高度（可能含对齐行），visible_height 为可见高度。
  bool present_nv12(const uint8_t* nv12, int width, int visible_height,
                    int coded_height, int stride) override;

  // GDI 兜底路径（无 GPU 时）。
  void present(const uint8_t* bgra, int width, int height, int stride) override;

  const char* name() const override {
    return swapchain_ ? "dxgi-swapchain" : "win32-gdi";
  }

 private:
  static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam,
                                      LPARAM lparam);

  bool create_swapchain(std::string* error);
  bool ensure_pipeline(int width, int coded_height, int visible_height, int out_width,
                       int out_height, std::string* error);
  void release_pipeline();
  void gdi_present(const uint8_t* bgra, int width, int height, int stride);

  HWND hwnd_ = nullptr;
  bool alive_ = false;
  bool warned_bgra_on_swapchain_ = false;

  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
  Microsoft::WRL::ComPtr<ID3D11VideoDevice> video_device_;
  Microsoft::WRL::ComPtr<ID3D11VideoContext> video_context_;
  Microsoft::WRL::ComPtr<IDXGISwapChain1> swapchain_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> backbuffer_;
  Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> enumerator_;
  Microsoft::WRL::ComPtr<ID3D11VideoProcessor> processor_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> nv12_texture_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> nv12_staging_;
  Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> input_view_;
  Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> output_view_;
  int backbuffer_width_ = 0;
  int backbuffer_height_ = 0;
  int input_width_ = 0;
  int input_height_ = 0;
  int input_visible_height_ = 0;
};

}  // namespace rc

#endif  // _WIN32
