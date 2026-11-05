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
};

}  // namespace rc

#endif  // __linux__
