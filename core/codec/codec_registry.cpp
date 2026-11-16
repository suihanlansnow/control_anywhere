#include "codec/codec_registry.h"

#include <mutex>

#include "base/log.h"
#include "capture/capture.h"
#include "codec/decoder.h"
#include "codec/encoder.h"

namespace rc {
namespace {

// 探测用的小尺寸帧：只要能真正产出一帧码流，就认为该编码器在本机可用。
// 用 640x480 是因为部分 H.264 MFT 要求宽高按 16 对齐，更小的非对齐尺寸会误判为不可用。
constexpr int kProbeWidth = 640;
constexpr int kProbeHeight = 480;

struct ProbeResult {
  bool done = false;
  bool available = false;
  bool hardware = false;
};

std::mutex g_probe_mutex;
ProbeResult g_encode_probe[2];
ProbeResult g_decode_probe[2];

int codec_index(const std::string& codec) {
  if (codec == "vp9") return 0;
  if (codec == "h264") return 1;
  return -1;
}

bool probe_encode(const std::string& codec, bool* hardware) {
  std::string error;
  auto encoder = create_encoder(codec, &error);
  if (!encoder) return false;
  if (!encoder->open(kProbeWidth, kProbeHeight, 30, 75, &error)) {
    RC_LOGW("codec", codec << " encode probe failed: " << error);
    return false;
  }

  CapturedFrame frame;
  frame.width = kProbeWidth;
  frame.height = kProbeHeight;
  frame.stride = kProbeWidth * 4;
  frame.format = PixelFormat::kBGRA;
  frame.capture_ts_us = 0;
  frame.pixels.resize(static_cast<std::size_t>(kProbeWidth) * kProbeHeight * 4);
  for (std::size_t i = 0; i < frame.pixels.size(); ++i) {
    frame.pixels[i] = static_cast<uint8_t>((i * 7) & 0xFF);
  }

  // 异步 MFT 首帧可能只被接收而不立刻出包，多喂几帧再判定。
  EncodedFrame out;
  bool produced = false;
  for (int i = 0; i < 5 && !produced; ++i) {
    frame.capture_ts_us = static_cast<int64_t>(i) * 33333;
    if (!encoder->encode(frame, out, &error)) continue;
    produced = !out.data.empty();
  }
  if (!produced) {
    RC_LOGW("codec", codec << " encode probe produced no frame: " << error);
    return false;
  }
  *hardware = encoder->is_hardware();
  return true;
}

bool probe_decode(const std::string& codec, bool* hardware) {
  std::string error;
  auto decoder = create_decoder(codec, &error);
  if (!decoder) return false;
  if (!decoder->open(&error)) {
    RC_LOGW("codec", codec << " decode probe failed: " << error);
    return false;
  }
  *hardware = decoder->is_hardware();
  return true;
}

ProbeResult& encode_slot(const std::string& codec) {
  static ProbeResult fallback;
  const int index = codec_index(codec);
  return index >= 0 ? g_encode_probe[index] : fallback;
}

ProbeResult& decode_slot(const std::string& codec) {
  static ProbeResult fallback;
  const int index = codec_index(codec);
  return index >= 0 ? g_decode_probe[index] : fallback;
}

std::vector<std::string> collect(bool encode) {
  std::vector<std::string> result;
  static const char* kOrder[] = {"vp9", "h264"};
  std::lock_guard<std::mutex> lock(g_probe_mutex);
  for (const char* codec : kOrder) {
    // H.264 是既有链路（Windows Media Foundation / Linux FFmpeg），系统必然可用；
    // 且 Windows 的异步 H.264 MFT 不适合离屏试编码。只有新增的 VP9 需要真实探测。
    if (std::string(codec) == "h264") {
      result.emplace_back(codec);
      continue;
    }
    ProbeResult& slot = encode ? encode_slot(codec) : decode_slot(codec);
    if (!slot.done) {
      slot.hardware = false;
      slot.available = encode ? probe_encode(codec, &slot.hardware)
                              : probe_decode(codec, &slot.hardware);
      slot.done = true;
      RC_LOGI("codec", codec << (encode ? " encoder=" : " decoder=")
                             << (slot.available ? (slot.hardware ? "hardware" : "software")
                                                : "unavailable"));
    }
    if (slot.available) result.emplace_back(codec);
  }
  return result;
}

}  // namespace

CodecAvailability codec_availability(const std::string& codec) {
  CodecAvailability result;
  // H.264 为既有链路，不参与探测（Windows 异步 MFT 离屏试编码可能卡住）。
  if (codec == "h264") {
    result.encode = true;
    result.decode = true;
    return result;
  }
  {
    std::lock_guard<std::mutex> lock(g_probe_mutex);
    ProbeResult& slot = encode_slot(codec);
    if (!slot.done) {
      slot.hardware = false;
      slot.available = probe_encode(codec, &slot.hardware);
      slot.done = true;
    }
    result.encode = slot.available;
    result.encode_hardware = slot.hardware;
  }
  {
    std::lock_guard<std::mutex> lock(g_probe_mutex);
    ProbeResult& slot = decode_slot(codec);
    if (!slot.done) {
      slot.hardware = false;
      slot.available = probe_decode(codec, &slot.hardware);
      slot.done = true;
    }
    result.decode = slot.available;
    result.decode_hardware = slot.hardware;
  }
  return result;
}

std::vector<std::string> encodable_codecs() { return collect(true); }

std::vector<std::string> decodable_codecs() { return collect(false); }

}  // namespace rc
