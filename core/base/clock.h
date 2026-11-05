#pragma once

#include <chrono>
#include <cstdint>

namespace rc {

inline uint64_t now_us() {
  using namespace std::chrono;
  return static_cast<uint64_t>(
      duration_cast<microseconds>(system_clock::now().time_since_epoch()).count());
}

inline uint32_t now_ms() {
  using namespace std::chrono;
  return static_cast<uint32_t>(
      duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

}  // namespace rc
