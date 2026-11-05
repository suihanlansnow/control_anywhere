#include "render/sdl_renderer.h"

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

  width_ = width;
  height_ = height;
  alive_ = true;
  RC_LOGI("sdl", "renderer opened " << width << "x" << height);
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

  if (SDL_UpdateTexture(texture_, nullptr, bgra, stride) != 0) {
    RC_LOGW("sdl", "SDL_UpdateTexture failed: " << SDL_GetError());
    return;
  }

  SDL_RenderClear(renderer_);
  SDL_RenderCopy(renderer_, texture_, nullptr, nullptr);
  SDL_RenderPresent(renderer_);
}

}  // namespace rc
