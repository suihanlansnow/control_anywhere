#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "capture/dxgi_capture.h"

#include <cstdio>
#include <cstring>
#include <iterator>
#include <thread>

#include "base/clock.h"
#include "base/log.h"

namespace rc {
namespace {

std::string hr_text(HRESULT hr) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "0x%08lX", static_cast<unsigned long>(hr));
  return buffer;
}

// 纹理直通时同时给编码器留下足够的在途帧。硬编 MFT 内部会缓存多帧输入，
// 池子必须大于这个深度，否则还没编完的纹理会被采集线程重新写入。
constexpr int kOutputSlotCount = 10;

}  // namespace

bool DxgiCapture::start(int monitor_index, std::string* error) {
  monitor_index_ = monitor_index;

  D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                D3D_FEATURE_LEVEL_10_0};
  const UINT sdk = D3D11_SDK_VERSION;
  // 编码器 MFT 要共享这块设备做零拷贝，最好带上 VIDEO_SUPPORT；
  // 个别驱动不接受该标志，退回普通设备（后续会因 ResetDevice 失败禁用直通）。
  HRESULT hr = D3D11CreateDevice(
      nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
      D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, levels,
      static_cast<UINT>(std::size(levels)), sdk, device_.GetAddressOf(), nullptr,
      context_.GetAddressOf());
  if (FAILED(hr)) {
    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                           D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                           static_cast<UINT>(std::size(levels)), sdk,
                           device_.GetAddressOf(), nullptr, context_.GetAddressOf());
  }
  if (FAILED(hr)) {
    if (error) *error = "D3D11CreateDevice failed " + hr_text(hr);
    return false;
  }

  return create_duplication(error);
}

bool DxgiCapture::create_duplication(std::string* error) {
  Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
  if (FAILED(device_.As(&dxgi_device))) {
    if (error) *error = "IDXGIDevice unavailable";
    return false;
  }

  Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
  if (FAILED(dxgi_device->GetAdapter(adapter.GetAddressOf()))) {
    if (error) *error = "adapter unavailable";
    return false;
  }

  Microsoft::WRL::ComPtr<IDXGIOutput> output;
  const HRESULT enum_hr = adapter->EnumOutputs(static_cast<UINT>(monitor_index_),
                                               output.GetAddressOf());
  if (FAILED(enum_hr)) {
    if (error) *error = "monitor " + std::to_string(monitor_index_) +
                        " not found " + hr_text(enum_hr);
    return false;
  }

  Microsoft::WRL::ComPtr<IDXGIOutput1> output1;
  if (FAILED(output.As(&output1))) {
    if (error) *error = "IDXGIOutput1 unavailable";
    return false;
  }

  duplication_.Reset();
  const HRESULT dup_hr = output1->DuplicateOutput(device_.Get(),
                                                  duplication_.GetAddressOf());
  if (FAILED(dup_hr)) {
    if (error) *error = "DuplicateOutput failed " + hr_text(dup_hr);
    return false;
  }
  return true;
}

void DxgiCapture::stop() {
  if (device_manager_ && device_handle_ != nullptr) {
    device_manager_->CloseDeviceHandle(device_handle_);
  }
  device_handle_ = nullptr;
  device_manager_.Reset();
  release_convert();
  duplication_.Reset();
  staging_.Reset();
  context_.Reset();
  device_.Reset();
}

bool DxgiCapture::set_device_manager(void* manager) {
  if (manager == nullptr) return false;
  if (device_manager_ && device_manager_.Get() == manager) return true;
  device_manager_ = static_cast<IMFDXGIDeviceManager*>(manager);
  if (device_handle_ != nullptr) {
    device_manager_->CloseDeviceHandle(device_handle_);
    device_handle_ = nullptr;
  }
  if (FAILED(device_manager_->OpenDeviceHandle(&device_handle_))) {
    device_handle_ = nullptr;
    device_manager_.Reset();
    RC_LOGW("capture", "OpenDeviceHandle failed");
    return false;
  }
  RC_LOGI("capture", "shared mf device manager for serialized gpu access");
  return true;
}

void DxgiCapture::lock_device() {
  if (!device_manager_ || device_handle_ == nullptr) return;
  void* locked = nullptr;
  if (FAILED(device_manager_->LockDevice(device_handle_, __uuidof(ID3D11Device),
                                         &locked, TRUE))) {
    RC_LOGW("capture", "LockDevice failed");
    return;
  }
  if (locked != nullptr) static_cast<IUnknown*>(locked)->Release();
  device_locked_ = true;
}

void DxgiCapture::unlock_device() {
  if (!device_locked_) return;
  device_locked_ = false;
  if (device_manager_ && device_handle_ != nullptr) {
    device_manager_->UnlockDevice(device_handle_, FALSE);
  }
}

bool DxgiCapture::set_output_format(PixelFormat format) {
  if (format == PixelFormat::kBGRA) {
    format_ = format;
    return true;
  }
  // 支持与否要到拿到真实分辨率后建管线才知道；失败时 grab 会退回 BGRA，
  // 每帧的 CapturedFrame::format 会如实反映实际格式。
  release_convert();
  convert_failed_ = false;
  format_ = format;
  return true;
}

bool DxgiCapture::set_texture_output(bool enabled) {
  if (enabled == texture_output_.load()) return true;
  texture_output_ = enabled;
  RC_LOGI("capture", "texture passthrough " << (enabled ? "enabled" : "disabled"));
  return true;
}

bool DxgiCapture::ensure_staging(int width, int height, DXGI_FORMAT format) {
  if (staging_ && width == width_ && height == height_ && format == staging_format_) {
    return true;
  }

  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = static_cast<UINT>(width);
  desc.Height = static_cast<UINT>(height);
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.SampleDesc.Quality = 0;
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  desc.MiscFlags = 0;

  staging_.Reset();
  if (FAILED(device_->CreateTexture2D(&desc, nullptr, staging_.GetAddressOf()))) {
    return false;
  }

  width_ = width;
  height_ = height;
  staging_format_ = format;
  return true;
}

void DxgiCapture::release_convert() {
  slots_.clear();
  cpu_output_view_.Reset();
  input_view_.Reset();
  nv12_staging_[0].Reset();
  nv12_staging_[1].Reset();
  gpu_done_query_[0].Reset();
  gpu_done_query_[1].Reset();
  nv12_texture_.Reset();
  copied_texture_.Reset();
  processor_.Reset();
  enumerator_.Reset();
  convert_width_ = 0;
  convert_height_ = 0;
}

bool DxgiCapture::ensure_convert(int width, int height, std::string* error) {
  if (processor_ && convert_width_ == width && convert_height_ == height) return true;
  if (!device_) {
    if (error) *error = "d3d11 device is not open";
    return false;
  }
  release_convert();

  if (FAILED(device_.As(&video_device_)) || !video_device_ ||
      FAILED(context_.As(&video_context_)) || !video_context_) {
    if (error) *error = "device does not support video processing";
    return false;
  }

  D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc{};
  desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
  desc.InputFrameRate.Numerator = 60;
  desc.InputFrameRate.Denominator = 1;
  desc.InputWidth = static_cast<UINT>(width);
  desc.InputHeight = static_cast<UINT>(height);
  desc.OutputFrameRate.Numerator = 60;
  desc.OutputFrameRate.Denominator = 1;
  desc.OutputWidth = static_cast<UINT>(width);
  desc.OutputHeight = static_cast<UINT>(height);
  desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

  HRESULT hr = video_device_->CreateVideoProcessorEnumerator(&desc,
                                                             enumerator_.GetAddressOf());
  if (FAILED(hr) || !enumerator_) {
    if (error) *error = "CreateVideoProcessorEnumerator failed " + hr_text(hr);
    return false;
  }
  UINT support = 0;
  if (FAILED(enumerator_->CheckVideoProcessorFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                                    &support)) ||
      (support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) == 0) {
    if (error) *error = "video processor cannot read bgra";
    return false;
  }
  support = 0;
  if (FAILED(enumerator_->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &support)) ||
      (support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT) == 0) {
    if (error) *error = "video processor cannot write nv12";
    return false;
  }
  hr = video_device_->CreateVideoProcessor(enumerator_.Get(), 0,
                                           processor_.GetAddressOf());
  if (FAILED(hr) || !processor_) {
    if (error) *error = "CreateVideoProcessor failed " + hr_text(hr);
    return false;
  }

  // 桌面复制出来的纹理不能直接当视频处理器输入，先拷进我们自己可绑定的纹理。
  D3D11_TEXTURE2D_DESC texture{};
  texture.Width = static_cast<UINT>(width);
  texture.Height = static_cast<UINT>(height);
  texture.MipLevels = 1;
  texture.ArraySize = 1;
  texture.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  texture.SampleDesc.Count = 1;
  texture.Usage = D3D11_USAGE_DEFAULT;
  texture.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
  hr = device_->CreateTexture2D(&texture, nullptr, copied_texture_.GetAddressOf());
  if (FAILED(hr) || !copied_texture_) {
    if (error) *error = "CreateTexture2D(bgra) failed " + hr_text(hr);
    return false;
  }

  D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input_desc{};
  input_desc.FourCC = 0;
  input_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
  input_desc.Texture2D.MipSlice = 0;
  input_desc.Texture2D.ArraySlice = 0;
  hr = video_device_->CreateVideoProcessorInputView(
      copied_texture_.Get(), enumerator_.Get(), &input_desc,
      input_view_.GetAddressOf());
  if (FAILED(hr) || !input_view_) {
    if (error) *error = "CreateVideoProcessorInputView failed " + hr_text(hr);
    return false;
  }

  // CPU 回读路径用的输出纹理 + 回读暂存
  texture.Format = DXGI_FORMAT_NV12;
  hr = device_->CreateTexture2D(&texture, nullptr, nv12_texture_.GetAddressOf());
  if (FAILED(hr) || !nv12_texture_) {
    if (error) *error = "CreateTexture2D(nv12) failed " + hr_text(hr);
    return false;
  }

  D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC output_desc{};
  output_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
  output_desc.Texture2D.MipSlice = 0;
  hr = video_device_->CreateVideoProcessorOutputView(
      nv12_texture_.Get(), enumerator_.Get(), &output_desc,
      cpu_output_view_.GetAddressOf());
  if (FAILED(hr) || !cpu_output_view_) {
    if (error) *error = "CreateVideoProcessorOutputView failed " + hr_text(hr);
    return false;
  }

  texture.Usage = D3D11_USAGE_STAGING;
  texture.BindFlags = 0;
  texture.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  for (int i = 0; i < 2; ++i) {
    hr = device_->CreateTexture2D(&texture, nullptr, nv12_staging_[i].GetAddressOf());
    if (FAILED(hr) || !nv12_staging_[i]) {
      if (error) *error = "CreateTexture2D(nv12 staging) failed " + hr_text(hr);
      return false;
    }
    D3D11_QUERY_DESC qdesc{};
    qdesc.Query = D3D11_QUERY_EVENT;
    qdesc.MiscFlags = 0;
    hr = device_->CreateQuery(&qdesc, gpu_done_query_[i].GetAddressOf());
    if (FAILED(hr) || !gpu_done_query_[i]) {
      if (error) *error = "CreateQuery(event) failed " + hr_text(hr);
      return false;
    }
  }
  staging_write_idx_ = 0;
  staging_pipeline_primed_ = false;

  // 纹理直通池：编码器释放后（use_count 回到 1）纹理才能被复用。
  texture.Usage = D3D11_USAGE_DEFAULT;
  texture.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
  texture.CPUAccessFlags = 0;
  slots_.resize(kOutputSlotCount);
  for (auto& slot : slots_) {
    hr = device_->CreateTexture2D(&texture, nullptr, slot.texture.GetAddressOf());
    if (FAILED(hr) || !slot.texture) {
      if (error) *error = "CreateTexture2D(nv12 slot) failed " + hr_text(hr);
      slots_.clear();
      return false;
    }
    hr = video_device_->CreateVideoProcessorOutputView(
        slot.texture.Get(), enumerator_.Get(), &output_desc, slot.view.GetAddressOf());
    if (FAILED(hr) || !slot.view) {
      if (error) *error = "CreateVideoProcessorOutputView(slot) failed " + hr_text(hr);
      slots_.clear();
      return false;
    }
    // handle 自带一份纹理引用，帧被真正释放时纹理才回收。
    Microsoft::WRL::ComPtr<ID3D11Texture2D> keep = slot.texture;
    slot.handle = std::shared_ptr<void>(static_cast<void*>(slot.texture.Get()),
                                       [keep](void*) mutable { keep.Reset(); });
  }

  video_context_->VideoProcessorSetStreamFrameFormat(
      processor_.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
  video_context_->VideoProcessorSetStreamOutputRate(
      processor_.Get(), 0, D3D11_VIDEO_PROCESSOR_OUTPUT_RATE_NORMAL, TRUE, nullptr);
  video_context_->VideoProcessorSetStreamAutoProcessingMode(processor_.Get(), 0,
                                                            FALSE);
  video_context_->VideoProcessorSetStreamDestRect(processor_.Get(), 0, FALSE,
                                                  nullptr);

  // 输入是 sRGB 全范围，输出按 BT.601 limited（16-235）——与编码/解码侧的约定一致。
  D3D11_VIDEO_PROCESSOR_COLOR_SPACE input_space{};
  input_space.Usage = 0;
  input_space.RGB_Range = 0;  // 0-255
  video_context_->VideoProcessorSetStreamColorSpace(processor_.Get(), 0,
                                                    &input_space);
  D3D11_VIDEO_PROCESSOR_COLOR_SPACE output_space{};
  output_space.Usage = 0;
  output_space.YCbCr_Matrix = 0;   // BT.601
  output_space.Nominal_Range = 1;  // 16-235
  video_context_->VideoProcessorSetOutputColorSpace(processor_.Get(),
                                                    &output_space);

  convert_width_ = width;
  convert_height_ = height;
  RC_LOGI("capture", "gpu convert ready " << width << "x" << height << " bgra -> nv12");
  return true;
}

bool DxgiCapture::blit_nv12(ID3D11VideoProcessorOutputView* view) {
  D3D11_VIDEO_PROCESSOR_STREAM stream{};
  stream.Enable = TRUE;
  stream.OutputIndex = 0;
  stream.InputFrameOrField = 0;
  stream.PastFrames = 0;
  stream.FutureFrames = 0;
  stream.pInputSurface = input_view_.Get();
  return SUCCEEDED(video_context_->VideoProcessorBlt(processor_.Get(), view, 0, 1,
                                                     &stream));
}

bool DxgiCapture::grab_nv12_texture(CapturedFrame& frame, std::string* error) {
  // 找一个编码器已经放手的纹理，避免覆盖还在被 MFT 读取的数据。
  int index = -1;
  for (int i = 0; i < static_cast<int>(slots_.size()); ++i) {
    if (slots_[i].handle.use_count() == 1) {
      index = i;
      break;
    }
  }
  if (index < 0) {
    if (error) *error = "no free nv12 slot";
    return false;
  }

  if (!blit_nv12(slots_[index].view.Get())) {
    if (error) *error = "VideoProcessorBlt failed";
    return false;
  }

  frame.width = convert_width_;
  frame.height = convert_height_;
  frame.stride = convert_width_;
  frame.format = PixelFormat::kNV12;
  frame.pixels.clear();
  frame.gpu_texture = slots_[index].handle;
  return true;
}

bool DxgiCapture::grab_nv12_cpu(CapturedFrame& frame, std::string* error) {
  const uint64_t begin_us = now_us();
  if (!blit_nv12(cpu_output_view_.Get())) {
    if (error) *error = "VideoProcessorBlt failed";
    return false;
  }

  const int write_idx = staging_write_idx_;
  const int read_idx = staging_pipeline_primed_ ? (1 - write_idx) : write_idx;

  // 写当前帧到 staging[write_idx]，并标记 GPU 完成事件。
  context_->CopyResource(nv12_staging_[write_idx].Get(), nv12_texture_.Get());
  context_->End(gpu_done_query_[write_idx].Get());
  const uint64_t convert_us = now_us() - begin_us;

  // 读 staging[read_idx]：首帧读当前 staging（会阻塞），后续帧读上一帧 staging
  // （GPU 已有一帧时间完成回读，Map 不再阻塞）。
  if (staging_pipeline_primed_) {
    while (context_->GetData(gpu_done_query_[read_idx].Get(), nullptr, 0, 0) != S_OK) {
      std::this_thread::yield();
    }
  }
  staging_pipeline_primed_ = true;

  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (FAILED(context_->Map(nv12_staging_[read_idx].Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
    if (error) *error = "Map(nv12 staging) failed";
    return false;
  }

  const int width = convert_width_;
  const int height = convert_height_;
  frame.width = width;
  frame.height = height;
  frame.stride = width;
  frame.format = PixelFormat::kNV12;
  frame.gpu_texture.reset();
  frame.pixels.resize(static_cast<std::size_t>(width) * height * 3 / 2);

  const uint8_t* src = static_cast<const uint8_t*>(mapped.pData);
  uint8_t* y_dst = frame.pixels.data();
  for (int y = 0; y < height; ++y) {
    std::memcpy(y_dst + static_cast<std::size_t>(y) * width,
                src + static_cast<std::size_t>(y) * mapped.RowPitch,
                static_cast<std::size_t>(width));
  }
  uint8_t* uv_dst = y_dst + static_cast<std::size_t>(width) * height;
  const uint8_t* uv_src = src + static_cast<std::size_t>(mapped.RowPitch) * height;
  for (int y = 0; y < height / 2; ++y) {
    std::memcpy(uv_dst + static_cast<std::size_t>(y) * width,
                uv_src + static_cast<std::size_t>(y) * mapped.RowPitch,
                static_cast<std::size_t>(width));
  }
  context_->Unmap(nv12_staging_[read_idx].Get(), 0);

  staging_write_idx_ = 1 - write_idx;

  stat_convert_us_ += convert_us;
  stat_readback_us_ += now_us() - begin_us - convert_us;
  return true;
}

bool DxgiCapture::grab(CapturedFrame& frame, int timeout_ms) {
  if (!duplication_) return false;

  const uint64_t t_begin = now_us();
  DXGI_OUTDUPL_FRAME_INFO info{};
  Microsoft::WRL::ComPtr<IDXGIResource> resource;
  const HRESULT acquire_hr =
      duplication_->AcquireNextFrame(static_cast<UINT>(timeout_ms), &info,
                                     resource.GetAddressOf());
  if (acquire_hr == DXGI_ERROR_WAIT_TIMEOUT) return false;

  if (acquire_hr == DXGI_ERROR_ACCESS_LOST) {
    RC_LOGW("capture", "desktop access lost, recreating duplication");
    std::string ignored;
    create_duplication(&ignored);
    return false;
  }

  if (FAILED(acquire_hr)) {
    RC_LOGW("capture", "AcquireNextFrame failed");
    return false;
  }

  Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
  if (FAILED(resource.As(&texture))) {
    duplication_->ReleaseFrame();
    return false;
  }

  D3D11_TEXTURE2D_DESC desc{};
  texture->GetDesc(&desc);
  const uint64_t acquire_us = now_us() - t_begin;

  const int width = static_cast<int>(desc.Width);
  const int height = static_cast<int>(desc.Height);

  // 采集侧的 GPU 调用要与 MFT 内部的 GPU 工作串行，否则同一块 D3D11 设备会被
  // 两个线程并发使用，在驱动层直接卡死（表现为两个线程都 0 CPU 阻塞）。
  DeviceLockGuard device_lock(*this);

  std::string convert_error;
  if (format_ == PixelFormat::kNV12 && !convert_failed_ &&
      !ensure_convert(width, height, &convert_error)) {
    RC_LOGW("capture", "gpu convert unavailable: " << convert_error
                                                   << ", fallback to bgra readback");
    convert_failed_ = true;
  }

  if (format_ == PixelFormat::kNV12 && !convert_failed_) {
    // 桌面复制纹理必须先拷进可绑定纹理，再交给视频处理器。
    context_->CopyResource(copied_texture_.Get(), texture.Get());
    duplication_->ReleaseFrame();
    frame.capture_ts_us = static_cast<int64_t>(now_us());

    const bool texture_mode = texture_output_.load();
    const bool ok = texture_mode ? grab_nv12_texture(frame, &convert_error)
                                 : grab_nv12_cpu(frame, &convert_error);
    if (!ok) {
      if (texture_mode && convert_error == "no free nv12 slot") {
        return false;  // 编码器还没放手，这一帧丢掉比踩坏纹理好
      }
      RC_LOGW("capture", "gpu convert failed: " << convert_error
                                                << ", fallback to bgra readback");
      convert_failed_ = true;
      return false;
    }

    ++stat_frames_;
    stat_acquire_us_ += acquire_us;
    if (stat_frames_ % 30 == 0) {
      RC_LOGI("capture", "stage avg: acquire=" << stat_acquire_us_ / stat_frames_
                                               << "us gpu=" << stat_convert_us_ / stat_frames_
                                               << "us readback="
                                               << stat_readback_us_ / stat_frames_
                                               << "us mode="
                                               << (texture_mode ? "texture" : "cpu"));
    }
    return true;
  }

  // ---- BGRA 回读兜底 ----
  const bool staging_ready = ensure_staging(width, height, desc.Format);
  if (!staging_ready) {
    duplication_->ReleaseFrame();
    RC_LOGE("capture", "failed to create staging texture");
    return false;
  }

  context_->CopyResource(staging_.Get(), texture.Get());
  duplication_->ReleaseFrame();

  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (FAILED(context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
    RC_LOGW("capture", "Map staging texture failed");
    return false;
  }

  frame.width = width;
  frame.height = height;
  frame.stride = frame.width * 4;
  frame.format = PixelFormat::kBGRA;
  frame.gpu_texture.reset();
  frame.capture_ts_us = static_cast<int64_t>(now_us());
  frame.pixels.resize(static_cast<size_t>(frame.width) * frame.height * 4);

  const uint8_t* src = static_cast<const uint8_t*>(mapped.pData);
  uint8_t* dst = frame.pixels.data();
  for (int y = 0; y < frame.height; ++y) {
    std::memcpy(dst, src, static_cast<size_t>(frame.stride));
    src += mapped.RowPitch;
    dst += frame.stride;
  }

  context_->Unmap(staging_.Get(), 0);
  return true;
}

}  // namespace rc
