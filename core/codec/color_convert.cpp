#include "codec/color_convert.h"

namespace rc {
namespace {

inline uint8_t clamp_byte(int value) {
  return static_cast<uint8_t>(value < 0 ? 0 : (value > 255 ? 255 : value));
}

}  // namespace

void nv12_to_bgra(const uint8_t* nv12, int width, int visible_height, int coded_height,
                  int stride, std::vector<uint8_t>& bgra) {
  if (nv12 == nullptr || width <= 0 || visible_height <= 0 || stride < width) return;
  if (coded_height < visible_height) coded_height = visible_height;

  bgra.resize(static_cast<std::size_t>(width) * visible_height * 4);
  const uint8_t* uv_plane = nv12 + static_cast<std::size_t>(stride) * coded_height;

  for (int y = 0; y < visible_height; ++y) {
    const uint8_t* y_row = nv12 + static_cast<std::size_t>(y) * stride;
    const uint8_t* uv_row = uv_plane + static_cast<std::size_t>(y / 2) * stride;
    uint8_t* dst = bgra.data() + static_cast<std::size_t>(y) * width * 4;
    for (int x = 0; x < width; ++x) {
      const int c = y_row[x] - 16;
      const int cb = uv_row[(x / 2) * 2] - 128;
      const int cr = uv_row[(x / 2) * 2 + 1] - 128;
      dst[x * 4 + 0] = clamp_byte((298 * c + 516 * cb + 128) >> 8);
      dst[x * 4 + 1] = clamp_byte((298 * c - 100 * cb - 208 * cr + 128) >> 8);
      dst[x * 4 + 2] = clamp_byte((298 * c + 409 * cr + 128) >> 8);
      dst[x * 4 + 3] = 255;
    }
  }
}

}  // namespace rc
