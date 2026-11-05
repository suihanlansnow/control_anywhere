#pragma once

#include <cstdint>
#include <vector>

namespace rc {

// NV12 -> BGRA（BT.601 limited range），输出 top-down、紧凑排布。
// visible_height 可能小于 coded_height（1080 常被编码对齐到 1088），多余的行会被裁掉。
void nv12_to_bgra(const uint8_t* nv12, int width, int visible_height, int coded_height,
                  int stride, std::vector<uint8_t>& bgra);

}  // namespace rc
