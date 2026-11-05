#pragma once

#ifdef _WIN32

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

namespace rc {

// D3D11 VideoProcessor：把解码器输出的 NV12 画面在 GPU 上转成 top-down BGRA。
// 输入/输出纹理与视频处理器按尺寸缓存复用，只有画面尺寸变化时才重建。
class VideoProcessorConverter {
 public:
  ~VideoProcessorConverter();

  bool open(std::string* error);
  void close();
  const char* name() const { return "d3d11-video-processor"; }

  // nv12: 编码高度的 NV12（stride 为行字节数，UV 平面紧跟 Y 平面）。
  // visible_height: 实际可见高度（1080 常被对齐到 1088），多出来的行会被裁掉。
  bool convert(const uint8_t* nv12, int width, int height, int visible_height,
               int stride, std::vector<uint8_t>& bgra, std::string* error);

 private:
  bool ensure_size(int width, int height, int visible_height, std::string* error);
  void release();

  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
  Microsoft::WRL::ComPtr<ID3D11VideoDevice> video_device_;
  Microsoft::WRL::ComPtr<ID3D11VideoContext> video_context_;
  Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> enumerator_;
  Microsoft::WRL::ComPtr<ID3D11VideoProcessor> processor_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> nv12_texture_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> nv12_staging_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> bgra_texture_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> bgra_staging_;
  Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> input_view_;
  Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> output_view_;
  int coded_width_ = 0;
  int coded_height_ = 0;
  int visible_height_ = 0;
};

}  // namespace rc

#endif  // _WIN32
