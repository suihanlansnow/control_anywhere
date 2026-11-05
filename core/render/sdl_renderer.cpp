#include "render/sdl_renderer.h"

#include "base/clock.h"
#include "base/log.h"

namespace rc {

SdlRenderer::SdlRenderer() = default;

SdlRenderer::~SdlRenderer() { close(); }

bool SdlRenderer::open(const std::string& title, int width, int height,
                       std::string* error) {
  if (alive_) return true;

  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    if (error) *error = std::string("SDL_Init failed: ") + SDL_GetError();
    return false;
  }

  window_ = SDL_CreateWindow(
      title.c_str(),
      SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
      width, height,
      SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
  if (!window_) {
    if (error) *error = std::string("SDL_CreateWindow failed: ") + SDL_GetError();
    return false;
  }

  renderer_ = SDL_CreateRenderer(window_, -1,
                                  SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!renderer_) {
    RC_LOGW("sdl", "accelerated renderer not available, falling back to software");
    renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);
    if (!renderer_) {
      if (error) *error = std::string("SDL_CreateRenderer failed: ") + SDL_GetError();
      SDL_DestroyWindow(window_);
      window_ = nullptr;
      return false;
    }
  }

  texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_BGRA32,
                                SDL_TEXTUREACCESS_STREAMING, width, height);
  if (!texture_) {
    if (error) *error = std::string("SDL_CreateTexture failed: ") + SDL_GetError();
    close();
    return false;
  }

  // 取显示器刷新率作为上屏节拍；拿不到就按 60Hz 估计。
  const int display_index = SDL_GetWindowDisplayIndex(window_);
  if (display_index >= 0) {
    SDL_DisplayMode mode;
    if (SDL_GetCurrentDisplayMode(display_index, &mode) == 0 && mode.refresh_rate > 0) {
      refresh_hz_ = mode.refresh_rate;
    }
  }

  width_ = width;
  height_ = height;
  last_present_us_ = 0;
  alive_ = true;
  RC_LOGI("sdl", "renderer opened " << width << "x" << height << " @" << refresh_hz_ << "Hz");
  return true;
}

void SdlRenderer::close() {
  alive_ = false;
  if (texture_) { SDL_DestroyTexture(texture_); texture_ = nullptr; }
  if (renderer_) { SDL_DestroyRenderer(renderer_); renderer_ = nullptr; }
  if (window_) { SDL_DestroyWindow(window_); window_ = nullptr; }
  SDL_Quit();
}

void SdlRenderer::pump() {
  SDL_Event event;
  while (SDL_PollEvent(&event)) {
    if (event.type == SDL_QUIT || event.type == SDL_WINDOWEVENT_CLOSE) {
      alive_ = false;
    }
  }
}

void SdlRenderer::present(const uint8_t* bgra, int width, int height, int stride) {
  if (!alive_ || !texture_) return;

  // 距上次上屏不足一个刷新周期就直接丢弃这一帧：PRESENTVSYNC 会阻塞到下次垂直同步，
  // 若每帧都 present，解码循环会被拖慢到显示器刷新率以下。
  const uint64_t now = now_us();
  const uint64_t period_us = 1000000ULL / static_cast<uint64_t>(refresh_hz_ > 0 ? refresh_hz_ : 60);
  if (last_present_us_ != 0 && now - last_present_us_ < period_us) return;

  if (SDL_UpdateTexture(texture_, nullptr, bgra, stride) != 0) {
    RC_LOGW("sdl", "SDL_UpdateTexture failed: " << SDL_GetError());
    return;
  }

  SDL_RenderClear(renderer_);
  SDL_RenderCopy(renderer_, texture_, nullptr, nullptr);
  SDL_RenderPresent(renderer_);
  last_present_us_ = now;
}

}  // namespace rc
