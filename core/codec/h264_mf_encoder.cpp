#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "codec/h264_mf_encoder.h"

#include <d3d11.h>
#include <emmintrin.h>
#include <objbase.h>
#include <oleauto.h>

#include <cstdlib>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

#include "base/clock.h"
#include "base/log.h"

namespace rc {
namespace {

// H.264 profile_idc：Main = 77（codecapi.h 的 eAVEncH264VProfile_Main）。
constexpr UINT32 kH264ProfileMain = 77;
// evr.h 里的 MR_VIDEO_ACCELERATION_SERVICE；本地定义避免额外依赖 evr 头文件/库。
const GUID kVideoAccelerationService = {
    0xefef5175, 0x5c7d, 0x4ce2, {0xbb, 0xbd, 0x34, 0xff, 0x8b, 0xca, 0x65, 0x54}};
// 异步 MFT 的事件等待粒度；1080p60 每帧预算 16.7ms，这里给足余量。
constexpr int kWaitBudgetMs = 20;
// 输出短轮询预算：超过就留给下一帧，靠流水线隐藏 NVENC 的一帧延迟。
constexpr int kOutputPollMs = 4;

std::string hr_text(HRESULT hr) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "0x%08lX", static_cast<unsigned long>(hr));
  return buffer;
}

std::string wide_to_utf8(const wchar_t* text) {
  char buffer[256] = {};
  WideCharToMultiByte(CP_UTF8, 0, text, -1, buffer, sizeof(buffer) - 1, nullptr,
                      nullptr);
  return buffer;
}

bool has_nal_type_from(const uint8_t* data, std::size_t n, uint8_t nal_type) {
  for (std::size_t i = 0; i + 3 < n; ++i) {
    if (data[i] != 0 || data[i + 1] != 0) continue;
    if (data[i + 2] == 1) {
      if ((data[i + 3] & 0x1F) == nal_type) return true;
      i += 2;
    } else if (data[i + 2] == 0 && i + 4 < n && data[i + 3] == 1) {
      if ((data[i + 4] & 0x1F) == nal_type) return true;
      i += 3;
    }
  }
  return false;
}

bool has_nal_type(const std::vector<uint8_t>& data, uint8_t nal_type) {
  return has_nal_type_from(data.data(), data.size(), nal_type);
}

}  // namespace

H264MfEncoder::~H264MfEncoder() { close(); }

bool H264MfEncoder::open(int width, int height, int fps, int quality,
                         std::string* error) {
  if (transform_) close();

  const HRESULT com_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  com_initialized_ = SUCCEEDED(com_hr) || com_hr == RPC_E_CHANGED_MODE;

  if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
    if (error) *error = "MFStartup failed";
    return false;
  }
  mf_started_ = true;

  if (!activate(true, error)) {
    RC_LOGW("codec", "no hardware h264 mft, falling back to software encoder");
    if (!activate(false, error)) return false;
  }

  // 纹理直通默认关闭：NVENC 的 MFT 在输入纹理被客户端写入后会停摆（见 setup_d3d_manager）。
  // 关闭时也不共享设备，省掉采集端每帧的设备加锁。
  if (shared_device_ && std::getenv("RC_TEXTURE_PASSTHROUGH") != nullptr &&
      !setup_d3d_manager()) {
    RC_LOGW("codec", "gpu texture passthrough unavailable, encoder uses cpu frames");
  }

  if (!configure(width, height, fps, quality, error)) {
    close();
    return false;
  }
  return true;
}

bool H264MfEncoder::activate(bool hardware_only, std::string* error) {
  MFT_REGISTER_TYPE_INFO input_info{MFMediaType_Video, MFVideoFormat_NV12};
  MFT_REGISTER_TYPE_INFO output_info{MFMediaType_Video, MFVideoFormat_H264};

  IMFActivate** activates = nullptr;
  UINT32 count = 0;
  const UINT32 flags =
      MFT_ENUM_FLAG_SORTANDFILTER |
      (hardware_only ? MFT_ENUM_FLAG_HARDWARE : MFT_ENUM_FLAG_ALL);
  const HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, &input_info,
                               &output_info, &activates, &count);
  if (FAILED(hr) || activates == nullptr || count == 0) {
    if (activates) CoTaskMemFree(activates);
    if (error && error->empty()) {
      *error = hardware_only ? "no hardware h264 encoder" : "no h264 encoder mft";
    }
    return false;
  }

  bool ok = false;
  for (UINT32 i = 0; i < count && !ok; ++i) {
    Microsoft::WRL::ComPtr<IMFAttributes> attributes;
    MFCreateAttributes(attributes.GetAddressOf(), 2);
    // 异步 MFT 必须先解锁才能直接使用 IMFTransform 接口。
    attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
    attributes->SetUINT32(MF_LOW_LATENCY, TRUE);
    Microsoft::WRL::ComPtr<IMFTransform> transform;
    if (FAILED(activates[i]->ActivateObject(IID_PPV_ARGS(transform.GetAddressOf())))) {
      continue;
    }
    UINT32 async_flag = 0;
    Microsoft::WRL::ComPtr<IMFAttributes> transform_attributes;
    if (FAILED(transform->GetAttributes(transform_attributes.GetAddressOf())) ||
        !transform_attributes) {
      continue;
    }
    transform_attributes->SetUINT32(MF_LOW_LATENCY, TRUE);
    transform_attributes->GetUINT32(MF_TRANSFORM_ASYNC, &async_flag);
    // MF_TRANSFORM_ASYNC_UNLOCK 必须设在 MFT 自己的属性仓库上，设在 IMFActivate 上无效。
    if (async_flag != 0 &&
        FAILED(transform_attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE))) {
      continue;
    }
    hardware_ = hardware_only;
    async_ = async_flag != 0;
    events_.Reset();
    if (async_) transform.As(&events_);

    wchar_t friendly[128] = {};
    activates[i]->GetString(MFT_FRIENDLY_NAME_Attribute, friendly,
                            static_cast<UINT32>(sizeof(friendly) / sizeof(wchar_t)),
                            nullptr);
    RC_LOGI("codec", "using mft: " << wide_to_utf8(friendly)
                                   << " async=" << (async_ ? 1 : 0)
                                   << " hardware_pass=" << (hardware_ ? 1 : 0));

    transform_ = transform;
    ok = true;
  }

  for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
  CoTaskMemFree(activates);

  if (!ok) {
    if (error) *error = hardware_only ? "hardware h264 encoder unavailable"
                                      : "h264 encoder unavailable";
    return false;
  }
  return true;
}

bool H264MfEncoder::set_d3d_device(void* device) {
  if (device == nullptr) return false;
  // 这是采集端持有的设备指针，这里必须自己 AddRef（ComPtr 的构造函数会做），
  // 否则 close 时 Reset 会把采集端的引用一起吃掉，留下悬垂指针。
  shared_device_ =
      Microsoft::WRL::ComPtr<IUnknown>(static_cast<IUnknown*>(device));
  return true;
}

bool H264MfEncoder::setup_d3d_manager() {
  if (!shared_device_ || !transform_) return false;

  UINT reset_token = 0;
  if (FAILED(MFCreateDXGIDeviceManager(&reset_token,
                                       device_manager_.GetAddressOf())) ||
      !device_manager_) {
    return false;
  }
  // 采集和编码必须用同一块 D3D11 设备，纹理才能直接被 MFT 使用。
  if (FAILED(device_manager_->ResetDevice(shared_device_.Get(), reset_token))) {
    device_manager_.Reset();
    return false;
  }
  // 编码线程也要在这块设备上做 GPU 拷贝，所以单独开一个设备句柄。
  if (FAILED(device_manager_->OpenDeviceHandle(&device_handle_))) {
    device_handle_ = nullptr;
    device_manager_.Reset();
    return false;
  }
  transform_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                             reinterpret_cast<ULONG_PTR>(device_manager_.Get()));

  UINT32 aware = 0;
  Microsoft::WRL::ComPtr<IMFAttributes> attributes;
  if (SUCCEEDED(transform_->GetAttributes(attributes.GetAddressOf())) && attributes) {
    attributes->GetUINT32(MF_SA_D3D11_AWARE, &aware);
  }
  // 走到这里说明调用方显式启用了直通；剩下的交给 MFT 是否真的支持 D3D11 纹理。
  texture_input_ = aware != 0;
  RC_LOGI("codec", "d3d manager shared, mft_d3d11_aware=" << (aware ? 1 : 0)
                                                        << " texture_passthrough="
                                                        << (texture_input_ ? 1 : 0));
  return texture_input_;
}

bool H264MfEncoder::setup_sample_allocator() {
  if (!device_manager_ || !input_type_ || !transform_) return false;

  // NVENC 的 MFT 不提供自己的分配器（MR_VIDEO_ACCELERATION_SERVICE 返回
  // E_NOINTERFACE），只能退到通用分配器，用它分配的纹理才可能被硬编接受。
  HRESULT hr = MFGetService(transform_.Get(), kVideoAccelerationService,
                            IID_PPV_ARGS(sample_allocator_.GetAddressOf()));
  if (FAILED(hr) || !sample_allocator_) {
    hr = MFCreateVideoSampleAllocatorEx(
        IID_PPV_ARGS(sample_allocator_.GetAddressOf()));
  }
  if (FAILED(hr) || !sample_allocator_) {
    RC_LOGW("codec", "no video sample allocator " << hr_text(hr));
    return false;
  }
  if (FAILED(sample_allocator_->SetDirectXManager(device_manager_.Get()))) {
    sample_allocator_.Reset();
    return false;
  }

  Microsoft::WRL::ComPtr<IMFAttributes> attributes;
  if (FAILED(MFCreateAttributes(attributes.GetAddressOf(), 3)) || !attributes) {
    sample_allocator_.Reset();
    return false;
  }
  attributes->SetUINT32(MF_SA_D3D11_AWARE, 1);
  // NVENC 读写纹理需要 render target 绑定，部分驱动还要求 shader resource。
  attributes->SetUINT32(MF_SA_D3D11_BINDFLAGS,
                        D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);

  // 预分配 4 帧、最多 8 帧：硬编内部会缓存 2~3 帧输入。
  hr = sample_allocator_->InitializeSampleAllocatorEx(4, 8, attributes.Get(),
                                                      input_type_.Get());
  if (FAILED(hr)) {
    RC_LOGW("codec", "InitializeSampleAllocatorEx failed " << hr_text(hr));
    sample_allocator_.Reset();
    return false;
  }
  RC_LOGI("codec", "mft sample allocator ready (4..8 samples, d3d11 texture)");
  return true;
}

bool H264MfEncoder::configure(int width, int height, int fps, int quality,
                              std::string* error) {
  width_ = width & ~1;  // NV12 要求偶数尺寸
  height_ = height & ~1;
  if (width_ < 16 || height_ < 16) {
    if (error) *error = "frame too small for nv12";
    return false;
  }
  fps_ = fps > 0 ? fps : 30;

  const double pixels_per_second =
      static_cast<double>(width_) * height_ * fps_;
  const double bits_per_pixel = 0.04 + 0.08 * (std::clamp(quality, 1, 100) / 100.0);
  bitrate_ = static_cast<uint32_t>(pixels_per_second * bits_per_pixel);
  bitrate_ = std::clamp<uint32_t>(bitrate_, 1000000u, 40000000u);

  // ---- 输出类型：H.264 ——
  Microsoft::WRL::ComPtr<IMFMediaType> output_type;
  HRESULT hr = MFCreateMediaType(output_type.GetAddressOf());
  if (FAILED(hr)) {
    if (error) *error = "MFCreateMediaType failed " + hr_text(hr);
    return false;
  }
  output_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  output_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
  output_type->SetUINT32(MF_MT_AVG_BITRATE, bitrate_);
  output_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  output_type->SetUINT32(MF_MT_MPEG2_PROFILE, kH264ProfileMain);
  output_type->SetUINT32(MF_MT_MAX_KEYFRAME_SPACING, static_cast<UINT32>(fps_));
  MFSetAttributeSize(output_type.Get(), MF_MT_FRAME_SIZE,
                     static_cast<UINT32>(width_), static_cast<UINT32>(height_));
  MFSetAttributeRatio(output_type.Get(), MF_MT_FRAME_RATE,
                      static_cast<UINT32>(fps_), 1);
  MFSetAttributeRatio(output_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

  hr = transform_->SetOutputType(0, output_type.Get(), 0);
  if (FAILED(hr)) {
    // 部分硬编只接受它自己枚举出来的类型，按可用类型逐个回退。
    bool accepted = false;
    for (UINT32 i = 0; !accepted; ++i) {
      Microsoft::WRL::ComPtr<IMFMediaType> candidate;
      if (transform_->GetOutputAvailableType(0, i, candidate.GetAddressOf()) != S_OK) break;
      GUID subtype{};
      candidate->GetGUID(MF_MT_SUBTYPE, &subtype);
      if (subtype != MFVideoFormat_H264) continue;
      UINT32 candidate_w = 0, candidate_h = 0;
      MFGetAttributeSize(candidate.Get(), MF_MT_FRAME_SIZE, &candidate_w, &candidate_h);
      if (candidate_w != 0 && candidate_w != static_cast<UINT32>(width_)) continue;
      candidate->SetUINT32(MF_MT_AVG_BITRATE, bitrate_);
      candidate->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
      MFSetAttributeSize(candidate.Get(), MF_MT_FRAME_SIZE,
                         static_cast<UINT32>(width_), static_cast<UINT32>(height_));
      MFSetAttributeRatio(candidate.Get(), MF_MT_FRAME_RATE,
                          static_cast<UINT32>(fps_), 1);
      MFSetAttributeRatio(candidate.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
      if (transform_->SetOutputType(0, candidate.Get(), 0) == S_OK) accepted = true;
    }
    if (!accepted) {
      if (error) *error = "SetOutputType(h264) failed " + hr_text(hr);
      return false;
    }
  }

  // ---- 输入类型：NV12 ——
  Microsoft::WRL::ComPtr<IMFMediaType> input_type;
  hr = MFCreateMediaType(input_type.GetAddressOf());
  if (FAILED(hr)) {
    if (error) *error = "MFCreateMediaType failed " + hr_text(hr);
    return false;
  }
  input_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  input_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
  input_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  MFSetAttributeSize(input_type.Get(), MF_MT_FRAME_SIZE,
                     static_cast<UINT32>(width_), static_cast<UINT32>(height_));
  MFSetAttributeRatio(input_type.Get(), MF_MT_FRAME_RATE,
                      static_cast<UINT32>(fps_), 1);
  MFSetAttributeRatio(input_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
  hr = transform_->SetInputType(0, input_type.Get(), 0);
  if (FAILED(hr)) {
    if (error) *error = "SetInputType(nv12) failed " + hr_text(hr);
    return false;
  }

  // 强制关键帧走可选接口 ICodecAPI；不支持时请求会被拒绝并回落到等下一个 IDR。
  if (FAILED(transform_.As(&codec_api_))) codec_api_.Reset();

  // 直通需要 MFT 认可的样本分配器；建不出来就退回 CPU 帧（不冒险送裸纹理）。
  if (texture_input_) {
    input_type_ = input_type;
    if (!setup_sample_allocator()) {
      texture_input_ = false;
      RC_LOGW("codec", "texture passthrough disabled: no usable sample allocator");
    }
  }

  // ---- 启动流，取 SPS/PPS 与输出缓冲规格 ——
  transform_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
  transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
  transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

  Microsoft::WRL::ComPtr<IMFMediaType> current_output;
  if (SUCCEEDED(transform_->GetOutputCurrentType(0, current_output.GetAddressOf()))) {
    UINT32 blob_size = 0;
    if (SUCCEEDED(current_output->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &blob_size)) &&
        blob_size > 0) {
      sequence_header_.resize(blob_size);
      current_output->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, sequence_header_.data(),
                              blob_size, nullptr);
    }
  }

  MFT_OUTPUT_STREAM_INFO stream_info{};
  hr = transform_->GetOutputStreamInfo(0, &stream_info);
  if (FAILED(hr)) {
    if (error) *error = "GetOutputStreamInfo failed " + hr_text(hr);
    return false;
  }
  mft_provides_samples_ =
      (stream_info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
  output_buffer_size_ = stream_info.cbSize;
  if (output_buffer_size_ == 0) {
    output_buffer_size_ = static_cast<uint32_t>(width_) * height_ + 65536;
  }

  // NV12 平面 + 输入样本池（异步 MFT 可能同时持有 1~2 帧）。
  nv12_.resize(static_cast<std::size_t>(width_) * height_ * 3 / 2);
  input_samples_.clear();
  input_index_ = 0;
  has_input_token_ = false;
  has_output_token_ = false;
  frame_index_ = 0;
  held_peak_ = 0;
  cpu_miss_ = 0;
  held_samples_.clear();
  pending_capture_ts_.clear();
  for (int i = 0; i < 3; ++i) {
    Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
    MFCreateMemoryBuffer(static_cast<DWORD>(nv12_.size()), buffer.GetAddressOf());
    Microsoft::WRL::ComPtr<IMFSample> sample;
    MFCreateSample(sample.GetAddressOf());
    sample->AddBuffer(buffer.Get());
    input_samples_.push_back(sample);
  }

  // 输出样本池：MFT 不提供样本时复用，省掉每帧的 COM 分配。
  output_samples_.clear();
  output_index_ = 0;
  if (!mft_provides_samples_) {
    for (int i = 0; i < 3; ++i) {
      Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
      MFCreateMemoryBuffer(output_buffer_size_, buffer.GetAddressOf());
      Microsoft::WRL::ComPtr<IMFSample> sample;
      MFCreateSample(sample.GetAddressOf());
      sample->AddBuffer(buffer.Get());
      output_samples_.push_back(sample);
    }
  }

  RC_LOGI("codec", "h264 mft ready: " << width_ << "x" << height_ << "@" << fps_
                                      << " bitrate=" << (bitrate_ / 1000) << "kbps async="
                                      << (async_ ? 1 : 0) << " hw=" << (hardware_ ? 1 : 0)
                                      << " seq_header=" << sequence_header_.size() << "B");
  return true;
}

bool H264MfEncoder::convert_nv12(const CapturedFrame& source, std::string* error) {
  if (source.width != width_ || source.height != height_) {
    if (error) {
      *error = "size mismatch, capture " + std::to_string(source.width) + "x" +
               std::to_string(source.height) + " vs encoder " + std::to_string(width_) +
               "x" + std::to_string(height_);
    }
    return false;
  }

  if (source.format == PixelFormat::kNV12) {
    // 采集端已在 GPU 上转好 NV12，这里只需搬进 MFT 的输入缓冲（约 3MB）。
    const std::size_t need = static_cast<std::size_t>(width_) * height_ * 3 / 2;
    if (source.pixels.size() < need) {
      if (error) *error = "nv12 frame too small";
      return false;
    }
    std::memcpy(nv12_.data(), source.pixels.data(), need);
    return true;
  }

  const int stride = source.stride;
  uint8_t* y_plane = nv12_.data();
  uint8_t* uv_plane = nv12_.data() + static_cast<std::size_t>(width_) * height_;

  // Y 平面：SSE2 每轮处理 8 像素（BT.601 limited range）。
  {
    const __m128i zero = _mm_setzero_si128();
    const __m128i mask = _mm_set1_epi32(0x000000FF);
    const __m128i c_r = _mm_set1_epi16(66);
    const __m128i c_g = _mm_set1_epi16(130);
    const __m128i c_b = _mm_set1_epi16(25);
    const __m128i c_offset = _mm_set1_epi16(16);

    for (int y = 0; y < height_; ++y) {
      const uint8_t* src =
          source.pixels.data() + static_cast<std::size_t>(y) * stride;
      uint8_t* y_dst = y_plane + static_cast<std::size_t>(y) * width_;
      int x = 0;
      for (; x + 8 <= width_; x += 8) {
        __m128i lo = _mm_loadu_si128(
            reinterpret_cast<const __m128i*>(src + static_cast<std::size_t>(x) * 4));
        __m128i hi = _mm_loadu_si128(
            reinterpret_cast<const __m128i*>(src + static_cast<std::size_t>(x) * 4 + 16));

        auto y4 = [&](__m128i px) -> __m128i {
          __m128i b = _mm_and_si128(px, mask);
          __m128i g = _mm_and_si128(_mm_srli_epi32(px, 8), mask);
          __m128i r = _mm_and_si128(_mm_srli_epi32(px, 16), mask);
          b = _mm_unpacklo_epi8(b, zero);
          g = _mm_unpacklo_epi8(g, zero);
          r = _mm_unpacklo_epi8(r, zero);
          __m128i sum = _mm_add_epi16(
              _mm_mullo_epi16(r, c_r),
              _mm_add_epi16(_mm_mullo_epi16(g, c_g),
                            _mm_mullo_epi16(b, c_b)));
          sum = _mm_add_epi16(_mm_srli_epi16(sum, 8), c_offset);
          return _mm_packus_epi16(sum, sum);
        };

        __m128i y_lo = y4(lo);
        __m128i y_hi = y4(hi);
        // y_lo 低 4 字节 = 前 4 个 Y；y_hi 低 4 字节 = 后 4 个 Y。
        __m128i y8 = _mm_unpacklo_epi32(y_lo, y_hi);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(y_dst + x), y8);
      }
      for (; x < width_; ++x) {
        const uint8_t* p = src + static_cast<std::size_t>(x) * 4;
        const int b = p[0], g = p[1], r = p[2];
        y_dst[x] = static_cast<uint8_t>(16 + (66 * r + 130 * g + 25 * b) / 256);
      }
    }
  }

  // UV 平面：2x2 平均后 BT.601。每 4 个像素出 1 组 UV，标量处理。
  for (int y = 0; y < height_; y += 2) {
    const uint8_t* row0 = source.pixels.data() + static_cast<std::size_t>(y) * stride;
    const uint8_t* row1 = row0 + stride;
    uint8_t* uv = uv_plane + static_cast<std::size_t>(y / 2) * width_;
    for (int x = 0; x < width_; x += 2) {
      const uint8_t* p00 = row0 + static_cast<std::size_t>(x) * 4;
      const uint8_t* p01 = p00 + 4;
      const uint8_t* p10 = row1 + static_cast<std::size_t>(x) * 4;
      const uint8_t* p11 = p10 + 4;

      const int r = (p00[2] + p01[2] + p10[2] + p11[2]) / 4;
      const int g = (p00[1] + p01[1] + p10[1] + p11[1]) / 4;
      const int b = (p00[0] + p01[0] + p10[0] + p11[0]) / 4;
      uv[x + 0] = static_cast<uint8_t>(128 + (-38 * r - 74 * g + 112 * b) / 256);
      uv[x + 1] = static_cast<uint8_t>(128 + (112 * r - 94 * g - 18 * b) / 256);
    }
  }
  return true;
}

bool H264MfEncoder::texture_sample(const std::shared_ptr<void>& texture, int width,
                                   int height, Microsoft::WRL::ComPtr<IMFSample>& sample,
                                   std::string* error) {
  if (!texture) {
    if (error) *error = "empty gpu texture";
    return false;
  }
  if (width != width_ || height != height_) {
    if (error) *error = "texture size mismatch";
    return false;
  }
  if (!sample_allocator_) {
    if (error) *error = "sample allocator unavailable";
    return false;
  }

  Microsoft::WRL::ComPtr<IMFSample> allocated;
  const HRESULT hr = sample_allocator_->AllocateSample(allocated.GetAddressOf());
  if (FAILED(hr) || !allocated) {
    if (error) *error = "AllocateSample failed " + hr_text(hr);
    return false;
  }
  if (!copy_to_sample(texture, allocated.Get(), error)) return false;

  sample = allocated;
  return true;
}

bool H264MfEncoder::copy_to_sample(const std::shared_ptr<void>& texture,
                                   IMFSample* sample, std::string* error) {
  Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
  if (FAILED(sample->GetBufferByIndex(0, buffer.GetAddressOf())) || !buffer) {
    if (error) *error = "allocated sample has no buffer";
    return false;
  }
  Microsoft::WRL::ComPtr<IMFDXGIBuffer> dxgi_buffer;
  if (FAILED(buffer.As(&dxgi_buffer)) || !dxgi_buffer) {
    if (error) *error = "allocated sample is not a dxgi texture";
    return false;
  }
  Microsoft::WRL::ComPtr<IUnknown> resource;
  UINT subresource = 0;
  const HRESULT res_hr =
      dxgi_buffer->GetResource(IID_PPV_ARGS(resource.GetAddressOf()));
  if (FAILED(res_hr) || !resource ||
      FAILED(dxgi_buffer->GetSubresourceIndex(&subresource))) {
    if (error) *error = "dxgi buffer resource unavailable " + hr_text(res_hr);
    return false;
  }
  Microsoft::WRL::ComPtr<ID3D11Texture2D> dst;
  if (FAILED(resource.As(&dst)) || !dst) {
    if (error) *error = "allocated resource is not a texture";
    return false;
  }
  if (!device_manager_ || device_handle_ == nullptr) {
    if (error) *error = "no device manager handle";
    return false;
  }

  // 拷贝必须走设备管理器加锁：MFT 内部也用同一块设备做 GPU 工作。
  void* locked = nullptr;
  const HRESULT lock_hr = device_manager_->LockDevice(
      device_handle_, __uuidof(ID3D11Device), &locked, TRUE);
  if (FAILED(lock_hr) || locked == nullptr) {
    if (error) *error = "LockDevice failed " + hr_text(lock_hr);
    return false;
  }
  bool ok = false;
  {
    Microsoft::WRL::ComPtr<IUnknown> locked_owner;
    locked_owner.Attach(static_cast<IUnknown*>(locked));
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    if (SUCCEEDED(locked_owner.As(&device)) && device) {
      Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
      device->GetImmediateContext(context.GetAddressOf());
      if (context) {
        // 采集纹理和样本纹理都有可能是纹理数组，按子资源拷贝最稳。
        context->CopySubresourceRegion(
            dst.Get(), subresource, 0, 0, 0,
            static_cast<ID3D11Texture2D*>(texture.get()), 0, nullptr);
        ok = true;
      }
    }
  }
  device_manager_->UnlockDevice(device_handle_, FALSE);
  if (!ok && error) *error = "CopySubresourceRegion failed";
  return ok;
}

bool H264MfEncoder::wait_event(uint32_t want, int timeout_ms) {
  if (!events_) return false;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  int spins = 0;
  for (;;) {
    Microsoft::WRL::ComPtr<IMFMediaEvent> event;
    const HRESULT hr = events_->GetEvent(MF_EVENT_FLAG_NO_WAIT, event.GetAddressOf());
    if (FAILED(hr) || !event) {
      if (std::chrono::steady_clock::now() >= deadline) return false;
      // 先让出时间片（驱动通常会立刻回事件），避免 Sleep(1) 的 15ms 定时器粒度。
      if (spins < 8) {
        ++spins;
        SwitchToThread();
      } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      continue;
    }
    MediaEventType type = MEUnknown;
    event->GetType(&type);
    if (type == want) return true;
    // 不是要等的事件也不能丢：记成令牌，后续直接消费。
    if (type == METransformNeedInput) has_input_token_ = true;
    if (type == METransformHaveOutput) has_output_token_ = true;
  }
}

bool H264MfEncoder::accept_input(IMFSample* prepared, std::string* error) {
  if (async_) {
    if (!has_input_token_) {
      if (!wait_event(METransformNeedInput, kWaitBudgetMs)) {
        if (error) *error = "encoder did not request input";
        return false;
      }
    }
    has_input_token_ = false;
  }

  Microsoft::WRL::ComPtr<IMFSample> sample;
  if (prepared != nullptr) {
    // GPU 纹理直通：不再经过 CPU 内存。
    sample = prepared;
  } else {
    sample = input_samples_[static_cast<std::size_t>(input_index_) % input_samples_.size()];
    ++input_index_;

    Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->GetBufferByIndex(0, buffer.GetAddressOf()))) {
      if (error) *error = "input sample lost its buffer";
      return false;
    }
    uint8_t* dest = nullptr;
    DWORD max_length = 0;
    DWORD current_length = 0;
    if (FAILED(buffer->Lock(&dest, &max_length, &current_length))) {
      if (error) *error = "input buffer lock failed";
      return false;
    }
    if (max_length < nv12_.size()) {
      buffer->Unlock();
      if (error) *error = "input buffer too small";
      return false;
    }
    std::memcpy(dest, nv12_.data(), nv12_.size());
    buffer->SetCurrentLength(static_cast<DWORD>(nv12_.size()));
    buffer->Unlock();
  }

  sample->SetSampleTime(static_cast<LONGLONG>(frame_index_) * 10000000LL / fps_);
  sample->SetSampleDuration(static_cast<LONGLONG>(10000000LL / fps_));

  HRESULT hr = transform_->ProcessInput(0, sample.Get(), 0);
  if (hr == MF_E_NOTACCEPTING) {
    EncodedFrame discard;
    if (produce_output(discard, kWaitBudgetMs, nullptr) && !held_samples_.empty()) {
      held_samples_.pop_front();
    }
    hr = transform_->ProcessInput(0, sample.Get(), 0);
  }
  if (FAILED(hr)) {
    if (error) *error = "ProcessInput failed " + hr_text(hr);
    return false;
  }
  ++frame_index_;
  return true;
}

bool H264MfEncoder::request_keyframe() {
  if (!transform_ || keyframe_unsupported_) return false;
  keyframe_requested_.store(true);
  return true;
}

void H264MfEncoder::apply_pending_keyframe() {
  if (!keyframe_requested_.exchange(false)) return;
  if (!codec_api_) {
    if (!keyframe_unsupported_) {
      keyframe_unsupported_ = true;
      RC_LOGW("codec", "encoder has no ICodecAPI, keyframe request ignored");
    }
    return;
  }
  VARIANT value;
  VariantInit(&value);
  value.vt = VT_UI4;
  value.ulVal = 1;
  const HRESULT hr = codec_api_->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &value);
  VariantClear(&value);
  if (FAILED(hr)) {
    keyframe_unsupported_ = true;
    RC_LOGW("codec", "force keyframe rejected " << hr_text(hr));
    return;
  }
  RC_LOGI("codec", "forced IDR on next frame");
}

bool H264MfEncoder::renegotiate(std::string* error) {
  Microsoft::WRL::ComPtr<IMFMediaType> type;
  if (FAILED(transform_->GetOutputAvailableType(0, 0, type.GetAddressOf()))) {
    if (error) *error = "stream change without available type";
    return false;
  }
  if (FAILED(transform_->SetOutputType(0, type.Get(), 0))) {
    if (error) *error = "SetOutputType after stream change failed";
    return false;
  }
  return true;
}

bool H264MfEncoder::produce_output(EncodedFrame& out, int timeout_ms,
                                   std::string* error) {
  if (async_) {
    if (!has_output_token_) {
      if (!wait_event(METransformHaveOutput, timeout_ms)) return false;
    }
    has_output_token_ = false;
  }

  for (int attempt = 0; attempt < 4; ++attempt) {
    MFT_OUTPUT_DATA_BUFFER buffer{};
    Microsoft::WRL::ComPtr<IMFSample> sample;
    if (!mft_provides_samples_) {
      if (output_samples_.empty()) {
        if (error) *error = "output sample pool not initialized";
        return false;
      }
      sample = output_samples_[output_index_ % output_samples_.size()];
      ++output_index_;
      buffer.pSample = sample.Get();
    }

    DWORD status = 0;
    const HRESULT hr = transform_->ProcessOutput(0, 1, &buffer, &status);

    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
      if (buffer.pSample) buffer.pSample->Release();
      if (buffer.pEvents) buffer.pEvents->Release();
      if (!renegotiate(error)) return false;
      continue;
    }
    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
      if (buffer.pSample) buffer.pSample->Release();
      if (buffer.pEvents) buffer.pEvents->Release();
      return false;
    }
    if (FAILED(hr)) {
      if (buffer.pSample) buffer.pSample->Release();
      if (buffer.pEvents) buffer.pEvents->Release();
      if (error) *error = "ProcessOutput failed " + hr_text(hr);
      return false;
    }

    Microsoft::WRL::ComPtr<IMFMediaBuffer> contiguous;
    if (FAILED(buffer.pSample->ConvertToContiguousBuffer(contiguous.GetAddressOf()))) {
      if (buffer.pSample) buffer.pSample->Release();
      if (buffer.pEvents) buffer.pEvents->Release();
      if (error) *error = "ConvertToContiguousBuffer failed";
      return false;
    }
    // 关键帧优先用 MFT 的 CleanPoint 标记（零扫描）；属性缺失时回退到 NAL 扫描。
    UINT32 clean_point = 0;
    const bool clean_point_ok =
        SUCCEEDED(buffer.pSample->GetUINT32(MFSampleExtension_CleanPoint, &clean_point));
    bool keyframe = clean_point_ok ? (clean_point != 0) : false;

    uint8_t* data = nullptr;
    DWORD max_length = 0;
    DWORD current_length = 0;
    if (FAILED(contiguous->Lock(&data, &max_length, &current_length)) ||
        current_length == 0) {
      contiguous->Unlock();
      if (buffer.pSample) buffer.pSample->Release();
      if (buffer.pEvents) buffer.pEvents->Release();
      if (error) *error = "empty h264 output";
      return false;
    }

    // 硬编通常自带 SPS/PPS；缺了就补上，保证关键帧可独立解码。
    // SPS 是否内联只在第一个关键帧判定一次并缓存，避免每帧扫描码流。
    if (!clean_point_ok) {
      keyframe = has_nal_type_from(data, current_length, 5);
    }
    bool prepend_sps = false;
    if (keyframe) {
      if (!inband_sps_ && !sequence_header_.empty()) {
        if (has_nal_type_from(data, current_length, 7)) {
          inband_sps_ = true;
        } else {
          prepend_sps = true;
        }
      } else if (inband_sps_) {
        prepend_sps = false;
      }
    }

    if (prepend_sps) {
      const std::size_t seq_len = sequence_header_.size();
      out.data.resize(seq_len + current_length);
      std::memcpy(out.data.data(), sequence_header_.data(), seq_len);
      std::memcpy(out.data.data() + seq_len, data, current_length);
    } else {
      out.data.assign(data, data + current_length);
    }
    contiguous->Unlock();

    if (buffer.pSample) buffer.pSample->Release();
    if (buffer.pEvents) buffer.pEvents->Release();

    out.codec = VideoCodec::H264;
    out.keyframe = keyframe;
    out.width = width_;
    out.height = height_;
    // 流水线里这次取回的是上一帧，时间戳按入队顺序回填。
    out.capture_ts_us = pending_capture_ts_.empty() ? now_us() : pending_capture_ts_.front();
    if (!pending_capture_ts_.empty()) pending_capture_ts_.pop_front();
    out.encode_ts_us = now_us();
    return true;
  }

  if (error) *error = "ProcessOutput retry exhausted";
  return false;
}

bool H264MfEncoder::encode(const CapturedFrame& source, EncodedFrame& out,
                           std::string* error) {
  if (!transform_) {
    if (error) *error = "encoder is not open";
    return false;
  }
  const uint64_t t_convert = now_us();
  Microsoft::WRL::ComPtr<IMFSample> gpu_sample;
  if (texture_input_) {
    if (!source.gpu_texture) {
      // 直通模式只接受纹理帧；采集端切到纹理模式前的这一两帧直接丢掉。
      // 连续丢太多说明采集端给不出纹理，永久退回 CPU 帧。
      if (++cpu_miss_ > 10) {
        texture_input_ = false;
        RC_LOGW("codec", "capture never produced gpu texture, disable passthrough");
      }
      return false;
    }
    cpu_miss_ = 0;
    if (!texture_sample(source.gpu_texture, source.width, source.height, gpu_sample,
                        error)) {
      // 直通失败就永久退回 CPU 帧（主循环会把采集端切回回读模式）。
      RC_LOGW("codec", "texture input failed: " << (error ? *error : "")
                                                << ", fall back to cpu frames");
      texture_input_ = false;
      gpu_sample.Reset();
      if (source.pixels.empty()) return false;
    }
  }
  if (!gpu_sample) {
    if (source.pixels.empty()) return false;
    if (!convert_nv12(source, error)) return false;
  }
  const uint64_t t_pull = now_us();

  // 先收上一帧的输出：此时 NVENC 已经编完，正常路径几乎不等待。
  // 取出输出后编码器才会重新发 NeedInput，所以顺序不能反。
  bool produced = produce_output(out, kOutputPollMs, nullptr);
  // 取回一帧输出，说明最早的那份输入样本已经用完，可以还给分配器。
  if (produced && !held_samples_.empty()) held_samples_.pop_front();
  // 关键帧请求必须在 ProcessInput 之前设置才会作用于这一帧。
  apply_pending_keyframe();
  const uint64_t t_input = now_us();
  if (!accept_input(gpu_sample.Get(), error)) {
    // 输入没被接收：把已回收的帧交出去，别丢。
    return produced;
  }
  if (gpu_sample) {
    // MFT 可能内部缓存多帧输入，样本必须持有到对应输出取回为止。
    held_samples_.push_back(gpu_sample);
    held_peak_ = std::max(held_peak_, held_samples_.size());
  }
  pending_capture_ts_.push_back(static_cast<uint64_t>(source.capture_ts_us));
  const uint64_t t_end = now_us();

  conv_us_ += t_pull - t_convert;
  output_us_ += t_input - t_pull;
  input_us_ += t_end - t_input;
  if (frame_index_ % 30 == 0) {
    RC_LOGI("codec", "stage avg: convert=" << (conv_us_ / frame_index_) << "us pull="
                                           << (output_us_ / frame_index_) << "us push="
                                           << (input_us_ / frame_index_) << "us in_flight="
                                           << held_samples_.size() << "/" << held_peak_);
  }

  if (!produced) {
    if (error && error->empty()) error->clear();
    return false;
  }
  out.encode_ts_us = now_us();
  return true;
}

void H264MfEncoder::close() {
  if (transform_) {
    transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    transform_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
  }
  input_samples_.clear();
  output_samples_.clear();
  output_index_ = 0;
  nv12_.clear();
  sequence_header_.clear();
  if (held_peak_ > 0) {
    RC_LOGI("codec", "texture in-flight peak=" << held_peak_);
  }
  held_samples_.clear();
  held_peak_ = 0;
  sample_allocator_.Reset();
  input_type_.Reset();
  codec_api_.Reset();
  keyframe_requested_.store(false);
  keyframe_unsupported_ = false;
  if (device_manager_ && device_handle_ != nullptr) {
    device_manager_->CloseDeviceHandle(device_handle_);
  }
  device_handle_ = nullptr;
  device_manager_.Reset();
  shared_device_.Reset();
  texture_input_ = false;
  inband_sps_ = false;
  events_.Reset();
  transform_.Reset();
  if (mf_started_) {
    MFShutdown();
    mf_started_ = false;
  }
  if (com_initialized_) {
    CoUninitialize();
    com_initialized_ = false;
  }
}

}  // namespace rc
