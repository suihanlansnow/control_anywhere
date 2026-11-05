#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "codec/d3d11_video_converter.h"

#include <cstdio>
#include <cstring>

#include "base/log.h"

namespace rc {
namespace {

std::string hr_text(HRESULT hr) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "0x%08lX", static_cast<unsigned long>(hr));
  return buffer;
}

}  // namespace

VideoProcessorConverter::~VideoProcessorConverter() { close(); }

bool VideoProcessorConverter::open(std::string* error) {
  if (device_) return true;

  static const D3D_FEATURE_LEVEL levels[] = {
      D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
      D3D_FEATURE_LEVEL_10_0};
  const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
  D3D_FEATURE_LEVEL obtained = D3D_FEATURE_LEVEL_10_0;

  HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                 levels, 4, D3D11_SDK_VERSION,
                                 device_.GetAddressOf(), &obtained,
                                 context_.GetAddressOf());
  if (hr == E_INVALIDARG) {
    // 老 SDK 不认识 11_1，去掉后重试。
    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                           levels + 1, 3, D3D11_SDK_VERSION,
                           device_.GetAddressOf(), &obtained,
                           context_.GetAddressOf());
  }
  if (FAILED(hr)) {
    // 无独显 / 远程桌面环境用 WARP 软设备兜底，VideoProcessor 依然可用。
    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels + 1,
                           3, D3D11_SDK_VERSION, device_.GetAddressOf(), &obtained,
                           context_.GetAddressOf());
  }
  if (FAILED(hr) || !device_ || !context_) {
    if (error) *error = "D3D11CreateDevice failed " + hr_text(hr);
    return false;
  }

  if (FAILED(device_.As(&video_device_)) || !video_device_ ||
      FAILED(context_.As(&video_context_)) || !video_context_) {
    if (error) *error = "device does not support video processing";
    close();
    return false;
  }

  RC_LOGI("codec", "d3d11 device ready, feature_level="
                       << static_cast<int>(obtained)
                       << (obtained == D3D_FEATURE_LEVEL_11_1 ? " (11_1)" : ""));
  return true;
}

void VideoProcessorConverter::release() {
  output_view_.Reset();
  input_view_.Reset();
  bgra_staging_.Reset();
  bgra_texture_.Reset();
  nv12_staging_.Reset();
  nv12_texture_.Reset();
  processor_.Reset();
  enumerator_.Reset();
  coded_width_ = 0;
  coded_height_ = 0;
  visible_height_ = 0;
}

void VideoProcessorConverter::close() {
  release();
  video_context_.Reset();
  video_device_.Reset();
  context_.Reset();
  device_.Reset();
}

bool VideoProcessorConverter::ensure_size(int width, int height, int visible_height,
                                          std::string* error) {
  if (processor_ && coded_width_ == width && coded_height_ == height &&
      visible_height_ == visible_height) {
    return true;
  }
  if (!device_) {
    if (error) *error = "d3d11 device is not open";
    return false;
  }
  release();

  D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc{};
  desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
  desc.InputFrameRate.Numerator = 60;
  desc.InputFrameRate.Denominator = 1;
  desc.InputWidth = static_cast<UINT>(width);
  desc.InputHeight = static_cast<UINT>(height);
  desc.OutputFrameRate.Numerator = 60;
  desc.OutputFrameRate.Denominator = 1;
  desc.OutputWidth = static_cast<UINT>(width);
  desc.OutputHeight = static_cast<UINT>(visible_height);
  desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

  HRESULT hr = video_device_->CreateVideoProcessorEnumerator(
      &desc, enumerator_.GetAddressOf());
  if (FAILED(hr) || !enumerator_) {
    if (error) *error = "CreateVideoProcessorEnumerator failed " + hr_text(hr);
    return false;
  }

  UINT support = 0;
  if (FAILED(enumerator_->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &support)) ||
      (support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) == 0) {
    if (error) *error = "video processor cannot read nv12";
    return false;
  }
  support = 0;
  if (FAILED(enumerator_->CheckVideoProcessorFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                                    &support)) ||
      (support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT) == 0) {
    if (error) *error = "video processor cannot write bgra";
    return false;
  }

  hr = video_device_->CreateVideoProcessor(enumerator_.Get(), 0,
                                           processor_.GetAddressOf());
  if (FAILED(hr) || !processor_) {
    if (error) *error = "CreateVideoProcessor failed " + hr_text(hr);
    return false;
  }

  // ---- 纹理：NV12 输入（default，供视频处理器读取）+ BGRA 输出（render target）----
  D3D11_TEXTURE2D_DESC texture{};
  texture.Width = static_cast<UINT>(width);
  texture.Height = static_cast<UINT>(height);
  texture.MipLevels = 1;
  texture.ArraySize = 1;
  texture.Format = DXGI_FORMAT_NV12;
  texture.SampleDesc.Count = 1;
  texture.Usage = D3D11_USAGE_DEFAULT;
  // D3D11 要求视频处理器输入资源带 RENDER_TARGET 绑定，只给 SHADER_RESOURCE 会
  // 在 CreateVideoProcessorInputView 时报 E_INVALIDARG。
  texture.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
  hr = device_->CreateTexture2D(&texture, nullptr, nv12_texture_.GetAddressOf());
  if (FAILED(hr) || !nv12_texture_) {
    if (error) *error = "CreateTexture2D(nv12) failed " + hr_text(hr);
    return false;
  }

  texture.Usage = D3D11_USAGE_STAGING;
  texture.BindFlags = 0;
  texture.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  hr = device_->CreateTexture2D(&texture, nullptr, nv12_staging_.GetAddressOf());
  if (FAILED(hr) || !nv12_staging_) {
    if (error) *error = "CreateTexture2D(nv12 staging) failed " + hr_text(hr);
    return false;
  }

  texture.Height = static_cast<UINT>(visible_height);
  texture.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  texture.Usage = D3D11_USAGE_DEFAULT;
  texture.BindFlags = D3D11_BIND_RENDER_TARGET;
  texture.CPUAccessFlags = 0;
  hr = device_->CreateTexture2D(&texture, nullptr, bgra_texture_.GetAddressOf());
  if (FAILED(hr) || !bgra_texture_) {
    if (error) *error = "CreateTexture2D(bgra) failed " + hr_text(hr);
    return false;
  }

  texture.Usage = D3D11_USAGE_STAGING;
  texture.BindFlags = 0;
  texture.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  hr = device_->CreateTexture2D(&texture, nullptr, bgra_staging_.GetAddressOf());
  if (FAILED(hr) || !bgra_staging_) {
    if (error) *error = "CreateTexture2D(bgra staging) failed " + hr_text(hr);
    return false;
  }

  // ---- 输入/输出视图 ----
  D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input_desc{};
  input_desc.FourCC = 0;
  input_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
  input_desc.Texture2D.MipSlice = 0;
  input_desc.Texture2D.ArraySlice = 0;
  hr = video_device_->CreateVideoProcessorInputView(
      nv12_texture_.Get(), enumerator_.Get(), &input_desc,
      input_view_.GetAddressOf());
  if (FAILED(hr) || !input_view_) {
    if (error) *error = "CreateVideoProcessorInputView failed " + hr_text(hr);
    return false;
  }

  D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC output_desc{};
  output_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
  output_desc.Texture2D.MipSlice = 0;
  hr = video_device_->CreateVideoProcessorOutputView(
      bgra_texture_.Get(), enumerator_.Get(), &output_desc,
      output_view_.GetAddressOf());
  if (FAILED(hr) || !output_view_) {
    if (error) *error = "CreateVideoProcessorOutputView failed " + hr_text(hr);
    return false;
  }

  // ---- 固定的流参数：逐行、不插帧、只做色彩空间转换，不做额外缩放/降噪 ----
  video_context_->VideoProcessorSetStreamFrameFormat(
      processor_.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
  video_context_->VideoProcessorSetStreamOutputRate(
      processor_.Get(), 0, D3D11_VIDEO_PROCESSOR_OUTPUT_RATE_NORMAL, TRUE, nullptr);
  video_context_->VideoProcessorSetStreamAutoProcessingMode(processor_.Get(), 0,
                                                            FALSE);
  RECT source_rect{0, 0, static_cast<LONG>(width),
                   static_cast<LONG>(visible_height)};
  video_context_->VideoProcessorSetStreamSourceRect(processor_.Get(), 0, TRUE,
                                                    &source_rect);
  video_context_->VideoProcessorSetStreamDestRect(processor_.Get(), 0, FALSE,
                                                  nullptr);

  // 编码端是 BT.601 limited range，这里必须显式声明，否则默认按 BT.709 会偏色。
  D3D11_VIDEO_PROCESSOR_COLOR_SPACE input_space{};
  input_space.Usage = 0;
  input_space.RGB_Range = 0;
  input_space.YCbCr_Matrix = 0;   // BT.601
  input_space.YCbCr_xvYCC = 0;
  input_space.Nominal_Range = 1;  // 16-235
  video_context_->VideoProcessorSetStreamColorSpace(processor_.Get(), 0,
                                                     &input_space);
  D3D11_VIDEO_PROCESSOR_COLOR_SPACE output_space{};
  output_space.Usage = 0;
  output_space.RGB_Range = 0;  // 0-255
  video_context_->VideoProcessorSetOutputColorSpace(processor_.Get(),
                                                    &output_space);

  coded_width_ = width;
  coded_height_ = height;
  visible_height_ = visible_height;
  RC_LOGI("codec", "video processor ready " << width << "x" << height << " -> "
                                             << width << "x" << visible_height);
  return true;
}

bool VideoProcessorConverter::convert(const uint8_t* nv12, int width, int height,
                                      int visible_height, int stride,
                                      std::vector<uint8_t>& bgra,
                                      std::string* error) {
  if (nv12 == nullptr || width <= 0 || height <= 0 || visible_height <= 0 ||
      visible_height > height || stride < width) {
    if (error) *error = "invalid nv12 input";
    return false;
  }
  if (!ensure_size(width, height, visible_height, error)) return false;

  // ---- 上传 NV12：staging -> default 纹理 ----
  D3D11_MAPPED_SUBRESOURCE mapped{};
  HRESULT hr = context_->Map(nv12_staging_.Get(), 0, D3D11_MAP_WRITE, 0, &mapped);
  if (FAILED(hr)) {
    if (error) *error = "Map(nv12 staging) failed " + hr_text(hr);
    return false;
  }
  uint8_t* dst = static_cast<uint8_t*>(mapped.pData);
  for (int y = 0; y < height; ++y) {
    std::memcpy(dst + static_cast<std::size_t>(y) * mapped.RowPitch,
                nv12 + static_cast<std::size_t>(y) * stride,
                static_cast<std::size_t>(width));
  }
  // staging 纹理只给一个 RowPitch，UV 平面紧跟在 Y 平面之后，共用同一个行距。
  uint8_t* uv_dst = dst + static_cast<std::size_t>(mapped.RowPitch) * height;
  const uint8_t* uv_src = nv12 + static_cast<std::size_t>(stride) * height;
  for (int y = 0; y < height / 2; ++y) {
    std::memcpy(uv_dst + static_cast<std::size_t>(y) * mapped.RowPitch,
                uv_src + static_cast<std::size_t>(y) * stride,
                static_cast<std::size_t>(width));
  }
  context_->Unmap(nv12_staging_.Get(), 0);
  context_->CopyResource(nv12_texture_.Get(), nv12_staging_.Get());

  // ---- GPU 转换 NV12 -> BGRA ----
  D3D11_VIDEO_PROCESSOR_STREAM stream{};
  stream.Enable = TRUE;
  stream.OutputIndex = 0;
  stream.InputFrameOrField = 0;
  stream.PastFrames = 0;
  stream.FutureFrames = 0;
  stream.pInputSurface = input_view_.Get();
  hr = video_context_->VideoProcessorBlt(processor_.Get(), output_view_.Get(), 0, 1,
                                         &stream);
  if (FAILED(hr)) {
    if (error) *error = "VideoProcessorBlt failed " + hr_text(hr);
    return false;
  }

  // ---- 回读：default -> staging，再拷贝成紧凑行 ----
  context_->CopyResource(bgra_staging_.Get(), bgra_texture_.Get());
  hr = context_->Map(bgra_staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
  if (FAILED(hr)) {
    if (error) *error = "Map(bgra staging) failed " + hr_text(hr);
    return false;
  }
  const int out_stride = width * 4;
  bgra.resize(static_cast<std::size_t>(out_stride) * visible_height);
  const uint8_t* src = static_cast<const uint8_t*>(mapped.pData);
  for (int y = 0; y < visible_height; ++y) {
    std::memcpy(bgra.data() + static_cast<std::size_t>(y) * out_stride,
                src + static_cast<std::size_t>(y) * mapped.RowPitch, out_stride);
  }
  context_->Unmap(bgra_staging_.Get(), 0);
  return true;
}

}  // namespace rc
