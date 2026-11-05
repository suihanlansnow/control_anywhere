#pragma once

#ifdef __linux__

#include <SDL2/SDL.h>

#include <cstdint>
#include <string>

#include "render/renderer.h"

namespace rc {

class SdlRenderer : public IRenderer {
 public:
  SdlRenderer();
  ~SdlRenderer() override;

  bool open(const std::string& title, int width, int height,
            std::string* error) override;
  void close() override;
  bool alive() const override { return alive_; }
  bool gpu() const override { return true; }

  void pump() override;
  void present(const uint8_t* bgra, int width, int height, int stride) override;

  const char* name() const override { return "sdl2"; }

 private:
  SDL_Window* window_ = nullptr;
  SDL_Renderer* renderer_ = nullptr;
  SDL_Texture* texture_ = nullptr;
  bool alive_ = false;
  int width_ = 0;
  int height_ = 0;
  // 上屏节流：PRESENTVSYNC 下每帧都 RenderPresent 会阻塞解码循环，
  // 按显示器刷新率丢帧，只上屏“最新一帧”。
  int refresh_hz_ = 60;
  uint64_t last_present_us_ = 0;
};

}  // namespace rc

#endif  // __linux__
