#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "codec/h264_decoder.h"

#include <objbase.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "base/clock.h"
#include "base/log.h"
#include "codec/color_convert.h"
#include "codec/d3d11_video_converter.h"

namespace rc {
namespace {

std::string hr_text(HRESULT hr) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "0x%08lX", static_cast<unsigned long>(hr));
  return buffer;
}

bool is_rgb32(const GUID& subtype) {
  return subtype == MFVideoFormat_RGB32 || subtype == MFVideoFormat_ARGB32;
}

// MFVideoArea: OffsetX(4B) + OffsetY(4B) + Area(SIZE, 8B)，用于取真正可见的画面区域。
struct VideoAreaBlob {
  uint16_t offset_x_fract;
  int16_t offset_x;
  uint16_t offset_y_fract;
  int16_t offset_y;
  int32_t width;
  int32_t height;
};
static_assert(sizeof(VideoAreaBlob) == 16, "MFVideoArea layout mismatch");

// 1080 的视频解码输出常被对齐到 1088，多出来的行没有有效像素，按 aperture 裁掉。
int visible_height_of(IMFMediaType* type, int coded_height) {
  const GUID* attributes[] = {&MF_MT_MINIMUM_DISPLAY_APERTURE,
                              &MF_MT_GEOMETRIC_APERTURE};
  for (const GUID* guid : attributes) {
    VideoAreaBlob area{};
    UINT32 size = 0;
    if (FAILED(type->GetBlobSize(*guid, &size)) || size < sizeof(VideoAreaBlob)) {
      continue;
    }
    if (FAILED(type->GetBlob(*guid, reinterpret_cast<UINT8*>(&area),
                             sizeof(VideoAreaBlob), nullptr))) {
      continue;
    }
    if (area.height > 0 && area.height <= coded_height) {
      return static_cast<int>(area.height);
    }
  }
  return coded_height;
}

}  // namespace

H264Decoder::H264Decoder() = default;
H264Decoder::~H264Decoder() { close(); }

bool H264Decoder::open(std::string* error) {
  if (transform_) return true;

  const HRESULT com_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  com_initialized_ = SUCCEEDED(com_hr) || com_hr == RPC_E_CHANGED_MODE;
  if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
    if (error) *error = "MFStartup failed";
    return false;
  }
  mf_started_ = true;

  MFT_REGISTER_TYPE_INFO input_info{MFMediaType_Video, MFVideoFormat_H264};
  IMFActivate** activates = nullptr;
  UINT32 count = 0;
  // 解码侧不需要异步管线，只要同步 MFT，省掉事件循环。
  const HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
                               MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                               &input_info, nullptr, &activates, &count);
  if (FAILED(hr) || activates == nullptr || count == 0) {
    if (activates) CoTaskMemFree(activates);
    if (error) *error = "no synchronous h264 decoder mft " + hr_text(hr);
    return false;
  }

  bool activated = false;
  for (UINT32 i = 0; i < count && !activated; ++i) {
    Microsoft::WRL::ComPtr<IMFTransform> transform;
    if (FAILED(activates[i]->ActivateObject(IID_PPV_ARGS(transform.GetAddressOf())))) {
      continue;
    }
    wchar_t friendly[128] = {};
    activates[i]->GetString(MFT_FRIENDLY_NAME_Attribute, friendly,
                            static_cast<UINT32>(sizeof(friendly) / sizeof(wchar_t)),
                            nullptr);
    // 远控场景不能容忍解码器为了重排出帧而缓存几十帧，必须显式要求低延迟。
    Microsoft::WRL::ComPtr<IMFAttributes> transform_attributes;
    if (SUCCEEDED(transform->GetAttributes(transform_attributes.GetAddressOf())) &&
        transform_attributes) {
      transform_attributes->SetUINT32(MF_LOW_LATENCY, TRUE);
    }
    Microsoft::WRL::ComPtr<IMFMediaType> input_type;
    if (FAILED(MFCreateMediaType(input_type.GetAddressOf()))) continue;
    input_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    input_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    input_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (FAILED(transform->SetInputType(0, input_type.Get(), 0))) continue;

    char friendly_utf8[256] = {};
    WideCharToMultiByte(CP_UTF8, 0, friendly, -1, friendly_utf8,
                        static_cast<int>(sizeof(friendly_utf8) - 1), nullptr, nullptr);
    RC_LOGI("codec", "using decoder mft: " << friendly_utf8);
    transform_ = transform;
    activated = true;
  }

  for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
  CoTaskMemFree(activates);

  if (!activated) {
    if (error) *error = "h264 decoder could not accept input type";
    return false;
  }

  transform_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
  transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
  transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

  // 刻意不在这里配输出类型：微软解码器在没见到码流前给出的类型是 1920x1080，
  // 等真正解码后会要求改成 1920x1088，这次重协商会把已缓存的帧全部丢弃。
  // 因此等第一帧送进去、ProcessOutput 报 STREAM_CHANGE 时再挑选输出类型。
  return true;
}

bool H264Decoder::pick_output_type(std::string* error) {
  // 需要 GPU 直出时优先要 NV12（不做任何转换）；否则优先 RGB32（省掉一次 CPU 转换）。
  for (int pass = 0; pass < 2; ++pass) {
    const bool want_rgb = want_nv12_ ? pass == 1 : pass == 0;
    for (UINT32 i = 0;; ++i) {
      Microsoft::WRL::ComPtr<IMFMediaType> type;
      if (transform_->GetOutputAvailableType(0, i, type.GetAddressOf()) != S_OK) break;
      GUID subtype{};
      type->GetGUID(MF_MT_SUBTYPE, &subtype);
      const bool rgb32 = is_rgb32(subtype);
      if (want_rgb != rgb32) continue;
      if (!want_rgb && subtype != MFVideoFormat_NV12) continue;
      if (transform_->SetOutputType(0, type.Get(), 0) != S_OK) continue;

      UINT32 w = 0, h = 0;
      MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &w, &h);
      width_ = static_cast<int>(w);
      height_ = static_cast<int>(h);
      visible_height_ = visible_height_of(type.Get(), height_);
      output_rgb32_ = rgb32;

      // RGB32 默认可能是负 stride（bottom-up），这里记下真实值，输出时再翻正。
      INT32 stride = static_cast<INT32>(w) * (rgb32 ? 4 : 1);
      type->GetUINT32(MF_MT_DEFAULT_STRIDE, reinterpret_cast<UINT32*>(&stride));
      if (stride == 0) stride = static_cast<INT32>(w) * (rgb32 ? 4 : 1);
      output_stride_ = static_cast<int>(stride);

      MFT_OUTPUT_STREAM_INFO info{};
      provides_samples_ = false;
      output_buffer_size_ = 0;
      if (SUCCEEDED(transform_->GetOutputStreamInfo(0, &info)) && info.cbSize > 0) {
        output_buffer_size_ = info.cbSize;
        provides_samples_ = (info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
      } else {
        output_buffer_size_ = static_cast<uint32_t>(width_) * height_ * 4 + 4096;
      }
      RC_LOGI("codec", "decoder output " << width_ << "x" << height_ << " visible="
                                         << visible_height_
                                         << (rgb32 ? " rgb32" : " nv12")
                                         << " stride=" << output_stride_);
      return true;
    }
  }
  if (error) *error = "no rgb32/nv12 output type available";
  return false;
}

bool H264Decoder::pull_output(DecodedImage& out, std::string* error) {
  const uint64_t begin_us = now_us();
  MFT_OUTPUT_DATA_BUFFER output{};
  Microsoft::WRL::ComPtr<IMFSample> allocated;
  if (!provides_samples_ && output_buffer_size_ == 0) {
    // 输出类型还没定：先空跑一次 ProcessOutput 触发类型协商，再按可用类型挑选。
    MFT_OUTPUT_DATA_BUFFER probe{};
    DWORD probe_status = 0;
    const HRESULT probe_hr = transform_->ProcessOutput(0, 1, &probe, &probe_status);
    if (probe.pSample) probe.pSample->Release();
    if (probe.pEvents) probe.pEvents->Release();
    if (probe_hr == MF_E_TRANSFORM_STREAM_CHANGE ||
        probe_hr == MF_E_TRANSFORM_TYPE_NOT_SET) {
      if (!pick_output_type(error)) return false;
      return pull_output(out, error);
    }
    if (probe_hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return false;
    if (FAILED(probe_hr)) {
      if (error) *error = "ProcessOutput(type probe) failed " + hr_text(probe_hr);
      return false;
    }
  }
  if (!provides_samples_) {
    if (output_buffer_size_ == 0) {
      if (error) *error = "decoder output buffer size unknown";
      return false;
    }
    Microsoft::WRL::ComPtr<IMFMediaBuffer> memory;
    if (FAILED(MFCreateMemoryBuffer(output_buffer_size_, memory.GetAddressOf())) ||
        FAILED(MFCreateSample(allocated.GetAddressOf())) ||
        FAILED(allocated->AddBuffer(memory.Get()))) {
      if (error) *error = "output buffer alloc failed";
      return false;
    }
    output.pSample = allocated.Get();
  }

  DWORD status = 0;
  const HRESULT hr = transform_->ProcessOutput(0, 1, &output, &status);

  // pSample 若由 MFT 提供，归我们释放；否则由 allocated 托管，不能重复释放。
  Microsoft::WRL::ComPtr<IMFSample> produced;
  if (provides_samples_ && output.pSample) produced.Attach(output.pSample);
  Microsoft::WRL::ComPtr<IMFCollection> events;
  if (output.pEvents) events.Attach(output.pEvents);

  if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
    if (!pick_output_type(error)) return false;
    return pull_output(out, error);
  }
  if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
    return false;  // 需要更多码流才能出图，不算错误
  }
  if (FAILED(hr)) {
    if (error) *error = "ProcessOutput failed " + hr_text(hr);
    return false;
  }

  IMFSample* result = produced ? produced.Get() : allocated.Get();
  if (result == nullptr) {
    if (error) *error = "decoder returned empty sample";
    return false;
  }

  Microsoft::WRL::ComPtr<IMFMediaBuffer> contiguous;
  uint8_t* bits = nullptr;
  DWORD max_length = 0;
  DWORD current_length = 0;
  bool locked = false;
  if (SUCCEEDED(result->ConvertToContiguousBuffer(contiguous.GetAddressOf())) &&
      SUCCEEDED(contiguous->Lock(&bits, &max_length, &current_length)) &&
      current_length > 0) {
    locked = true;
  }
  if (!locked) {
    if (contiguous) contiguous->Unlock();
    if (error) *error = "decoded buffer unavailable";
    return false;
  }
  const uint64_t convert_begin_us = now_us();
  const int visible = visible_height_ > 0 ? visible_height_ : height_;

  if (output_rgb32_) {
    out.nv12 = false;
    out.width = width_;
    out.height = height_;
    out.stride = width_ * 4;
    out.pixels.resize(static_cast<std::size_t>(width_) * height_ * 4);
    const int src_stride = output_stride_;
    for (int y = 0; y < height_; ++y) {
      // stride 为负说明是 bottom-up 布局，第 y 行（从上数）在内存里的位置要反过来算。
      const int src_y = src_stride >= 0 ? y : (height_ - 1 - y);
      const uint8_t* src = bits + static_cast<std::size_t>(src_y) * std::abs(src_stride);
      std::memcpy(out.pixels.data() + static_cast<std::size_t>(y) * width_ * 4, src,
                  static_cast<std::size_t>(width_) * 4);
    }
  } else if (want_nv12_) {
    // GPU 上屏路径：直接把 NV12 平面交出去，不做颜色转换、也不回读。
    out.nv12 = true;
    out.width = width_;
    out.height = visible;
    out.coded_height = height_;
    out.stride = width_;
    out.pixels.resize(static_cast<std::size_t>(width_) * height_ * 3 / 2);
    uint8_t* y_dst = out.pixels.data();
    for (int y = 0; y < height_; ++y) {
      std::memcpy(y_dst + static_cast<std::size_t>(y) * width_,
                  bits + static_cast<std::size_t>(y) * output_stride_,
                  static_cast<std::size_t>(width_));
    }
    uint8_t* uv_dst = y_dst + static_cast<std::size_t>(width_) * height_;
    const uint8_t* uv_src = bits + static_cast<std::size_t>(output_stride_) * height_;
    for (int y = 0; y < height_ / 2; ++y) {
      std::memcpy(uv_dst + static_cast<std::size_t>(y) * width_,
                  uv_src + static_cast<std::size_t>(y) * output_stride_,
                  static_cast<std::size_t>(width_));
    }
  } else {
    out.nv12 = false;
    // 优先用 D3D11 VideoProcessor 在 GPU 上做 NV12 -> BGRA，失败才回退 CPU。
    if (!converter_failed_ && !converter_) {
      converter_ = std::make_unique<VideoProcessorConverter>();
      std::string convert_error;
      if (converter_->open(&convert_error)) {
        RC_LOGI("codec", "nv12->bgra converter=" << converter_->name());
      } else {
        RC_LOGW("codec", "d3d11 converter unavailable: " << convert_error
                                                         << ", fallback to cpu");
        converter_.reset();
        converter_failed_ = true;
      }
    }

    bool converted = false;
    if (converter_) {
      std::string convert_error;
      converted = converter_->convert(bits, width_, height_, visible, output_stride_,
                                      out.pixels, &convert_error);
      if (!converted) {
        RC_LOGW("codec", "d3d11 convert failed: " << convert_error
                                                 << ", fallback to cpu");
        converter_->close();
        converter_.reset();
        converter_failed_ = true;
      }
    }
    if (converted) {
      out.width = width_;
      out.height = visible;
      out.stride = width_ * 4;
    } else {
      nv12_to_bgra(bits, width_, visible, height_, output_stride_, out.pixels);
      out.width = width_;
      out.height = visible;
      out.stride = width_ * 4;
    }
  }

  contiguous->Unlock();
  stat_decode_us_ += convert_begin_us - begin_us;
  stat_convert_us_ += now_us() - convert_begin_us;
  if (++stat_frames_ % 30 == 0) {
    RC_LOGI("codec", "stage avg: decode=" << stat_decode_us_ / stat_frames_
                                          << "us convert=" << stat_convert_us_ / stat_frames_
                                          << "us");
  }
  return true;
}

bool H264Decoder::decode(const uint8_t* data, std::size_t len, int64_t timestamp_us,
                         DecodedImage& out, std::string* error) {
  if (!transform_) {
    if (error) *error = "decoder is not open";
    return false;
  }
  if (data == nullptr || len == 0) {
    if (error) *error = "empty bitstream";
    return false;
  }

  // 复用输入样本：容量不够时按 1.5x 扩容，避免每帧 COM 分配。
  if (!input_sample_) {
    if (FAILED(MFCreateSample(input_sample_.GetAddressOf()))) {
      if (error) *error = "MFCreateSample failed";
      return false;
    }
  }
  if (!input_buffer_ || input_buffer_capacity_ < len) {
    const uint32_t new_cap =
        std::max(static_cast<uint32_t>(len), input_buffer_capacity_ * 3 / 2);
    Microsoft::WRL::ComPtr<IMFMediaBuffer> new_buffer;
    if (FAILED(MFCreateMemoryBuffer(new_cap, new_buffer.GetAddressOf()))) {
      if (error) *error = "MFCreateMemoryBuffer failed";
      return false;
    }
    input_buffer_ = new_buffer;
    input_buffer_capacity_ = new_cap;
    input_sample_->RemoveAllBuffers();
    input_sample_->AddBuffer(input_buffer_.Get());
  }

  uint8_t* dest = nullptr;
  DWORD max_length = 0;
  DWORD current_length = 0;
  if (FAILED(input_buffer_->Lock(&dest, &max_length, &current_length))) {
    if (error) *error = "input buffer lock failed";
    return false;
  }
  std::memcpy(dest, data, len);
  input_buffer_->SetCurrentLength(static_cast<DWORD>(len));
  input_buffer_->Unlock();

  input_sample_->SetSampleTime(timestamp_us * 10);
  input_sample_->SetSampleDuration(200000);

  // 送入码流。一旦解码器暂时不收，必须先把已解出的画面全部抽走再重送，
  // 否则这一帧码流会丢，后面的画面就会错位。
  std::string drain_error;
  HRESULT hr = transform_->ProcessInput(0, input_sample_.Get(), 0);
  if (hr == MF_E_NOTACCEPTING) {
    DecodedImage pending;
    while (pull_output(pending, &drain_error)) {
      pending.timestamp_us = static_cast<uint64_t>(timestamp_us);
      ready_.push_back(std::move(pending));
      pending = DecodedImage{};
    }
    hr = transform_->ProcessInput(0, input_sample_.Get(), 0);
  }
  if (FAILED(hr)) {
    if (error) *error = "ProcessInput failed " + hr_text(hr);
    return false;
  }

  // 把当前所有可取的画面都收进队列，保证输入与输出严格一一对应，不跳帧。
  DecodedImage picture;
  while (pull_output(picture, &drain_error)) {
    picture.timestamp_us = static_cast<uint64_t>(timestamp_us);
    ready_.push_back(std::move(picture));
    picture = DecodedImage{};
  }

  if (ready_.empty()) {
    // 解码器还在缓冲（首帧常见），或者这一帧没产出，不算错误。
    return false;
  }
  out = std::move(ready_.front());
  ready_.pop_front();
  return true;
}

void H264Decoder::close() {
  if (transform_) {
    transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    transform_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    transform_.Reset();
  }
  ready_.clear();
  converter_.reset();
  input_sample_.Reset();
  input_buffer_.Reset();
  input_buffer_capacity_ = 0;
  converter_failed_ = false;
  width_ = 0;
  height_ = 0;
  visible_height_ = 0;
  output_stride_ = 0;
  output_buffer_size_ = 0;
  provides_samples_ = false;
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
