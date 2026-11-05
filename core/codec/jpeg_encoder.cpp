#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "codec/jpeg_encoder.h"

#include <objbase.h>
#include <ocidl.h>
#include <oleauto.h>

#include <cstdio>
#include <cstring>

#include "base/clock.h"
#include "base/log.h"

namespace rc {
namespace {

std::string hr_text(HRESULT hr) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "0x%08lX", static_cast<unsigned long>(hr));
  return buffer;
}

}  // namespace

bool JpegEncoder::open(int width, int height, int fps, int quality,
                       std::string* error) {
  (void)fps;
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  const HRESULT hr = CoCreateInstance(
      CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
      IID_PPV_ARGS(factory_.GetAddressOf()));
  if (FAILED(hr)) {
    if (error) *error = "WIC factory unavailable " + hr_text(hr);
    return false;
  }

  width_ = width;
  height_ = height;
  quality_ = quality < 1 ? 1 : (quality > 100 ? 100 : quality);
  return true;
}

void JpegEncoder::close() {
  bgr_buffer_.clear();
  bgr_buffer_.shrink_to_fit();
  factory_.Reset();
}

bool JpegEncoder::encode(const CapturedFrame& source, EncodedFrame& out,
                         std::string* error) {
  if (!factory_) {
    if (error) *error = "encoder is not open";
    return false;
  }
  if (source.width <= 0 || source.height <= 0) {
    if (error) *error = "empty frame";
    return false;
  }

  const int width = source.width;
  const int height = source.height;
  width_ = width;
  height_ = height;

  // WIC JPEG 不处理 alpha，转成 24bpp BGR 可以避免 SetPixelFormat 强制换格式。
  const size_t bgr_stride = static_cast<size_t>(width) * 3;
  bgr_buffer_.resize(bgr_stride * static_cast<size_t>(height));

  for (int y = 0; y < height; ++y) {
    const uint8_t* src = source.pixels.data() +
                         static_cast<size_t>(y) * static_cast<size_t>(source.stride);
    uint8_t* dst = bgr_buffer_.data() + static_cast<size_t>(y) * bgr_stride;
    for (int x = 0; x < width; ++x) {
      dst[x * 3 + 0] = src[x * 4 + 0];
      dst[x * 3 + 1] = src[x * 4 + 1];
      dst[x * 3 + 2] = src[x * 4 + 2];
    }
  }

  HGLOBAL memory = GlobalAlloc(GHND, 0);
  if (memory == nullptr) {
    if (error) *error = "GlobalAlloc failed";
    return false;
  }

  IStream* stream = nullptr;
  if (FAILED(CreateStreamOnHGlobal(memory, TRUE, &stream)) || stream == nullptr) {
    GlobalFree(memory);
    if (error) *error = "CreateStreamOnHGlobal failed";
    return false;
  }

  bool ok = false;
  do {
    Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
    HRESULT hr = factory_->CreateEncoder(GUID_ContainerFormatJpeg, nullptr,
                                         encoder.GetAddressOf());
    if (FAILED(hr)) break;

    hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
    if (FAILED(hr)) break;

    Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
    Microsoft::WRL::ComPtr<IPropertyBag2> properties;
    hr = encoder->CreateNewFrame(frame.GetAddressOf(), properties.GetAddressOf());
    if (FAILED(hr)) break;

    // 质量必须在 Initialize 之前写入属性包，Initialize 时才生效。
    PROPBAG2 option = {};
    option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
    VARIANT quality;
    VariantInit(&quality);
    quality.vt = VT_R4;
    quality.fltVal = static_cast<float>(quality_) / 100.0f;
    properties->Write(1, &option, &quality);
    VariantClear(&quality);

    hr = frame->Initialize(properties.Get());
    if (FAILED(hr)) break;

    hr = frame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height));
    if (FAILED(hr)) break;

    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    hr = frame->SetPixelFormat(&format);
    if (FAILED(hr)) break;
    if (!IsEqualGUID(format, GUID_WICPixelFormat24bppBGR)) {
      if (error) *error = "WIC refused 24bppBGR output format";
      break;
    }

    hr = frame->WritePixels(static_cast<UINT>(height),
                            static_cast<UINT>(bgr_stride),
                            static_cast<UINT>(bgr_buffer_.size()),
                            bgr_buffer_.data());
    if (FAILED(hr)) break;

    hr = frame->Commit();
    if (FAILED(hr)) break;

    hr = encoder->Commit();
    if (FAILED(hr)) break;

    LARGE_INTEGER zero{};
    ULARGE_INTEGER end{};
    stream->Seek(zero, STREAM_SEEK_END, &end);

    HGLOBAL output_memory = nullptr;
    if (FAILED(GetHGlobalFromStream(stream, &output_memory)) ||
        output_memory == nullptr) {
      if (error) *error = "GetHGlobalFromStream failed";
      break;
    }

    const size_t size = static_cast<size_t>(end.QuadPart);
    void* data = GlobalLock(output_memory);
    if (data == nullptr || size == 0) {
      GlobalUnlock(output_memory);
      if (error) *error = "empty jpeg output";
      break;
    }

    out.data.resize(size);
    std::memcpy(out.data.data(), data, size);
    GlobalUnlock(output_memory);

    out.codec = VideoCodec::Jpeg;
    out.keyframe = true;  // 每帧都是独立可解码帧
    out.width = width;
    out.height = height;
    out.capture_ts_us = source.capture_ts_us;
    out.encode_ts_us = now_us();
    ok = true;
  } while (false);

  stream->Release();

  if (!ok && error && error->empty()) *error = "jpeg encode failed";
  return ok;
}

}  // namespace rc
