#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include "render/window_renderer.h"

#include <cstdio>
#include <cstring>
#include <string>

#include "base/log.h"

namespace rc {
namespace {

constexpr wchar_t kWindowClass[] = L"RcRemoteWindow";

std::string hr_text(HRESULT hr) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "0x%08lX", static_cast<unsigned long>(hr));
  return buffer;
}

std::wstring utf8_to_wide(const std::string& text) {
  if (text.empty()) return L"rc remote";
  const int needed = MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                         static_cast<int>(text.size()), nullptr, 0);
  std::wstring wide(static_cast<std::size_t>(needed), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                      wide.data(), needed);
  return wide;
}

}  // namespace

WindowRenderer::~WindowRenderer() { close(); }

bool WindowRenderer::open(const std::string& title, int width, int height,
                          std::string* error) {
  if (alive_) return true;
  if (width <= 0 || height <= 0) {
    width = 1280;
    height = 720;
  }

  static bool class_registered = false;
  HINSTANCE instance = GetModuleHandleW(nullptr);
  if (!class_registered) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &WindowRenderer::window_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
      if (error) *error = "RegisterClassEx failed: " + std::to_string(GetLastError());
      return false;
    }
    class_registered = true;
  }

  RECT rect{0, 0, width, height};
  AdjustWindowRectEx(&rect, WS_OVERLAPPEDWINDOW, FALSE, 0);
  hwnd_ = CreateWindowExW(0, kWindowClass, utf8_to_wide(title).c_str(),
                          WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                          rect.right - rect.left, rect.bottom - rect.top, nullptr,
                          nullptr, instance, this);
  if (hwnd_ == nullptr) {
    if (error) *error = "CreateWindowEx failed: " + std::to_string(GetLastError());
    return false;
  }

  ShowWindow(hwnd_, SW_SHOWNORMAL);
  UpdateWindow(hwnd_);
  alive_ = true;

  std::string gpu_error;
  if (!create_swapchain(&gpu_error)) {
    // 没有可用的 D3D11 设备时不致命，退回 GDI 显示（由解码器提供 BGRA）。
    RC_LOGW("render", "dxgi swapchain unavailable: " << gpu_error << ", fallback to gdi");
  }
  RC_LOGI("render", "window opened " << width << "x" << height << " display="
                                      << name());
  return true;
}

bool WindowRenderer::create_swapchain(std::string* error) {
  static const D3D_FEATURE_LEVEL levels[] = {
      D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
      D3D_FEATURE_LEVEL_10_0};
  D3D_FEATURE_LEVEL obtained = D3D_FEATURE_LEVEL_10_0;
  HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 4,
                                 D3D11_SDK_VERSION, device_.GetAddressOf(), &obtained,
                                 context_.GetAddressOf());
  if (hr == E_INVALIDARG) {
    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                           D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels + 1, 3,
                           D3D11_SDK_VERSION, device_.GetAddressOf(), &obtained,
                           context_.GetAddressOf());
  }
  if (FAILED(hr) || !device_ || !context_) {
    if (error) *error = "D3D11CreateDevice failed " + hr_text(hr);
    return false;
  }

  Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
  Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
  Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
  if (FAILED(device_.As(&dxgi_device)) || !dxgi_device ||
      FAILED(dxgi_device->GetAdapter(adapter.GetAddressOf())) || !adapter ||
      FAILED(adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf()))) || !factory) {
    if (error) *error = "cannot get dxgi factory";
    return false;
  }

  RECT client{};
  GetClientRect(hwnd_, &client);
  const int client_width = client.right - client.left;
  const int client_height = client.bottom - client.top;
  if (client_width <= 0 || client_height <= 0) {
    if (error) *error = "empty client area";
    return false;
  }

  DXGI_SWAP_CHAIN_DESC1 desc{};
  desc.Width = static_cast<UINT>(client_width);
  desc.Height = static_cast<UINT>(client_height);
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.Stereo = FALSE;
  desc.SampleDesc.Count = 1;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.BufferCount = 2;
  desc.Scaling = DXGI_SCALING_STRETCH;
  desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  desc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
  hr = factory->CreateSwapChainForHwnd(device_.Get(), hwnd_, &desc, nullptr, nullptr,
                                       swapchain_.GetAddressOf());
  if (FAILED(hr) || !swapchain_) {
    if (error) *error = "CreateSwapChainForHwnd failed " + hr_text(hr);
    return false;
  }
  // Alt+Enter 全屏会打乱窗口尺寸，远控窗口不需要。
  factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);

  if (FAILED(device_.As(&video_device_)) || !video_device_ ||
      FAILED(context_.As(&video_context_)) || !video_context_) {
    swapchain_.Reset();
    if (error) *error = "device does not support video processing";
    return false;
  }

  backbuffer_width_ = client_width;
  backbuffer_height_ = client_height;
  RC_LOGI("render", "swapchain ready " << client_width << "x" << client_height
                                       << " feature_level=" << static_cast<int>(obtained));
  return true;
}

void WindowRenderer::release_pipeline() {
  output_view_.Reset();
  input_view_.Reset();
  nv12_staging_.Reset();
  nv12_texture_.Reset();
  processor_.Reset();
  enumerator_.Reset();
  input_width_ = 0;
  input_height_ = 0;
  input_visible_height_ = 0;
}

void WindowRenderer::close() {
  release_pipeline();
  output_view_.Reset();
  backbuffer_.Reset();
  swapchain_.Reset();
  video_context_.Reset();
  video_device_.Reset();
  context_.Reset();
  device_.Reset();
  backbuffer_width_ = 0;
  backbuffer_height_ = 0;
  if (hwnd_ != nullptr) {
    if (IsWindow(hwnd_)) DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
  alive_ = false;
}

bool WindowRenderer::ensure_pipeline(int width, int coded_height, int visible_height,
                                     int out_width, int out_height,
                                     std::string* error) {
  if (processor_ && input_width_ == width && input_height_ == coded_height &&
      input_visible_height_ == visible_height && backbuffer_ &&
      backbuffer_width_ == out_width && backbuffer_height_ == out_height) {
    return true;
  }
  if (!swapchain_ || !video_device_ || !video_context_) {
    if (error) *error = "swapchain not ready";
    return false;
  }

  // ---- back buffer 尺寸跟随客户区 ----
  if (!backbuffer_ || backbuffer_width_ != out_width ||
      backbuffer_height_ != out_height) {
    output_view_.Reset();
    backbuffer_.Reset();
    HRESULT hr = swapchain_->ResizeBuffers(0, static_cast<UINT>(out_width),
                                           static_cast<UINT>(out_height),
                                           DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
      if (error) *error = "ResizeBuffers failed " + hr_text(hr);
      return false;
    }
    if (FAILED(swapchain_->GetBuffer(0, IID_PPV_ARGS(backbuffer_.GetAddressOf())))) {
      if (error) *error = "GetBuffer failed";
      return false;
    }
    backbuffer_width_ = out_width;
    backbuffer_height_ = out_height;
    RC_LOGI("render", "swapchain resized " << out_width << "x" << out_height);
  }

  release_pipeline();

  // ---- 处理器按“输入尺寸 + 输出尺寸”建立，输出尺寸变化会自动带来 GPU 缩放 ----
  D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc{};
  desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
  desc.InputFrameRate.Numerator = 60;
  desc.InputFrameRate.Denominator = 1;
  desc.InputWidth = static_cast<UINT>(width);
  desc.InputHeight = static_cast<UINT>(coded_height);
  desc.OutputFrameRate.Numerator = 60;
  desc.OutputFrameRate.Denominator = 1;
  desc.OutputWidth = static_cast<UINT>(out_width);
  desc.OutputHeight = static_cast<UINT>(out_height);
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

  // ---- NV12 输入纹理 + 上传用 staging ----
  D3D11_TEXTURE2D_DESC texture{};
  texture.Width = static_cast<UINT>(width);
  texture.Height = static_cast<UINT>(coded_height);
  texture.MipLevels = 1;
  texture.ArraySize = 1;
  texture.Format = DXGI_FORMAT_NV12;
  texture.SampleDesc.Count = 1;
  texture.Usage = D3D11_USAGE_DEFAULT;
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

  D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input_desc{};
  input_desc.FourCC = 0;
  input_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
  input_desc.Texture2D.MipSlice = 0;
  input_desc.Texture2D.ArraySlice = 0;
  hr = video_device_->CreateVideoProcessorInputView(nv12_texture_.Get(),
                                                    enumerator_.Get(), &input_desc,
                                                    input_view_.GetAddressOf());
  if (FAILED(hr) || !input_view_) {
    if (error) *error = "CreateVideoProcessorInputView failed " + hr_text(hr);
    return false;
  }

  D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC output_desc{};
  output_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
  output_desc.Texture2D.MipSlice = 0;
  hr = video_device_->CreateVideoProcessorOutputView(
      backbuffer_.Get(), enumerator_.Get(), &output_desc,
      output_view_.GetAddressOf());
  if (FAILED(hr) || !output_view_) {
    if (error) *error = "CreateVideoProcessorOutputView failed " + hr_text(hr);
    return false;
  }

  // ---- 固定流参数：只做色彩空间转换 + 缩放到客户区 ----
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

  // 编码端是 BT.601 limited range，必须显式声明，否则默认 BT.709 会偏色。
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
  video_context_->VideoProcessorSetOutputColorSpace(processor_.Get(), &output_space);

  input_width_ = width;
  input_height_ = coded_height;
  input_visible_height_ = visible_height;
  RC_LOGI("render", "video processor ready " << width << "x" << coded_height << " -> "
                                              << out_width << "x" << out_height);
  return true;
}

bool WindowRenderer::present_nv12(const uint8_t* nv12, int width, int visible_height,
                                  int coded_height, int stride) {
  if (!alive_ || hwnd_ == nullptr || swapchain_ == nullptr || nv12 == nullptr) {
    return false;
  }
  if (width <= 0 || visible_height <= 0 || coded_height < visible_height ||
      stride < width) {
    return false;
  }

  RECT client{};
  GetClientRect(hwnd_, &client);
  const int out_width = client.right - client.left;
  const int out_height = client.bottom - client.top;
  if (out_width <= 0 || out_height <= 0) return false;

  std::string error;
  if (!ensure_pipeline(width, coded_height, visible_height, out_width, out_height,
                       &error)) {
    RC_LOGW("render", "video processor pipeline failed: " << error);
    return false;
  }

  // ---- 上传 NV12（staging -> default）----
  D3D11_MAPPED_SUBRESOURCE mapped{};
  HRESULT hr = context_->Map(nv12_staging_.Get(), 0, D3D11_MAP_WRITE, 0, &mapped);
  if (FAILED(hr)) {
    RC_LOGW("render", "Map(nv12 staging) failed " << hr_text(hr));
    return false;
  }
  uint8_t* dst = static_cast<uint8_t*>(mapped.pData);
  for (int y = 0; y < coded_height; ++y) {
    std::memcpy(dst + static_cast<std::size_t>(y) * mapped.RowPitch,
                nv12 + static_cast<std::size_t>(y) * stride,
                static_cast<std::size_t>(width));
  }
  // staging 只有一个 RowPitch，UV 平面紧跟在 Y 平面之后。
  uint8_t* uv_dst = dst + static_cast<std::size_t>(mapped.RowPitch) * coded_height;
  const uint8_t* uv_src = nv12 + static_cast<std::size_t>(stride) * coded_height;
  for (int y = 0; y < coded_height / 2; ++y) {
    std::memcpy(uv_dst + static_cast<std::size_t>(y) * mapped.RowPitch,
                uv_src + static_cast<std::size_t>(y) * stride,
                static_cast<std::size_t>(width));
  }
  context_->Unmap(nv12_staging_.Get(), 0);
  context_->CopyResource(nv12_texture_.Get(), nv12_staging_.Get());

  // ---- 直接画进 back buffer，不经系统内存 ----
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
    RC_LOGW("render", "VideoProcessorBlt failed " << hr_text(hr));
    return false;
  }

  hr = swapchain_->Present(0, 0);
  if (FAILED(hr)) {
    RC_LOGW("render", "Present failed " << hr_text(hr));
    return false;
  }
  return true;
}

void WindowRenderer::present(const uint8_t* bgra, int width, int height, int stride) {
  if (!alive_ || hwnd_ == nullptr || bgra == nullptr || width <= 0 || height <= 0) {
    return;
  }
  if (swapchain_ != nullptr) {
    // flip 模型下窗口表面归 DXGI 所有，GDI 画上去不会显示。
    if (!warned_bgra_on_swapchain_) {
      warned_bgra_on_swapchain_ = true;
      RC_LOGW("render", "bgra frame on swapchain window ignored (expect nv12)");
    }
    return;
  }
  gdi_present(bgra, width, height, stride);
}

void WindowRenderer::gdi_present(const uint8_t* bgra, int width, int height,
                                 int stride) {
  RECT client{};
  GetClientRect(hwnd_, &client);
  const int dst_width = client.right - client.left;
  const int dst_height = client.bottom - client.top;
  if (dst_width <= 0 || dst_height <= 0) return;

  HDC dc = GetDC(hwnd_);
  if (dc == nullptr) return;

  BITMAPINFO bitmap{};
  bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bitmap.bmiHeader.biWidth = width;
  bitmap.bmiHeader.biHeight = -height;  // 负高度 = top-down，与解码输出一致
  bitmap.bmiHeader.biPlanes = 1;
  bitmap.bmiHeader.biBitCount = 32;
  bitmap.bmiHeader.biCompression = BI_RGB;

  // HALFTONE 缩放需要配 SetBrushOrgEx，否则部分驱动会退化到 COLORONCOLOR。
  SetStretchBltMode(dc, HALFTONE);
  SetBrushOrgEx(dc, 0, 0, nullptr);
  StretchDIBits(dc, 0, 0, dst_width, dst_height, 0, 0, width, height, bgra, &bitmap,
                DIB_RGB_COLORS, SRCCOPY);
  ReleaseDC(hwnd_, dc);
}

void WindowRenderer::pump() {
  if (!alive_) return;
  MSG msg{};
  while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
    if (msg.message == WM_QUIT) {
      alive_ = false;
      break;
    }
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  if (hwnd_ != nullptr && !IsWindow(hwnd_)) {
    hwnd_ = nullptr;
    alive_ = false;
  }
}

LRESULT CALLBACK WindowRenderer::window_proc(HWND hwnd, UINT message, WPARAM wparam,
                                             LPARAM lparam) {
  switch (message) {
    case WM_CREATE: {
      const CREATESTRUCTW* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(create->lpCreateParams));
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;  // 由 WM_PAINT 统一填黑，避免闪烁
    case WM_PAINT: {
      PAINTSTRUCT paint{};
      HDC dc = BeginPaint(hwnd, &paint);
      FillRect(dc, &paint.rcPaint, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
      EndPaint(hwnd, &paint);
      return 0;
    }
    case WM_CLOSE:
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY: {
      WindowRenderer* self =
          reinterpret_cast<WindowRenderer*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
      if (self != nullptr) self->alive_ = false;
      return 0;
    }
    default:
      break;
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

}  // namespace rc
