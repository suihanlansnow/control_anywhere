#pragma once

// VP9 解码：优先尝试平台硬件解码（D3D11VA / VAAPI / CUDA），失败才回退 FFmpeg 软解。
#if defined(RC_HAVE_FFMPEG) && RC_HAVE_FFMPEG

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <cstdint>
#include <deque>
#include <string>

#include "codec/decoder.h"

namespace rc {

class FFmpegVp9Decoder : public IVideoDecoder {
 public:
  FFmpegVp9Decoder();
  ~FFmpegVp9Decoder() override;

  void set_output(Output output) override { want_bgra_ = output == Output::kBgra; }

  bool open(std::string* error) override;
  void close() override;
  const char* name() const override {
    return is_hw_ ? "vp9-ffmpeg-hw" : "vp9-ffmpeg";
  }
  bool is_hardware() const override { return is_hw_; }

  bool decode(const uint8_t* data, std::size_t len, int64_t timestamp_us,
              DecodedImage& out, std::string* error) override;

 private:
  bool try_hardware(AVHWDeviceType device_type, AVPixelFormat hw_format,
                    std::string* error);
  bool open_software(std::string* error);
  bool init_sws(int width, int height, AVPixelFormat src_format);
  bool convert_frame(AVFrame* frame, int64_t timestamp_us, DecodedImage& out,
                     std::string* error);
  // 运行中硬件路径失效时重建软解，避免持续丢帧。
  bool fallback_to_software(std::string* error);
  void release_decoder();

  const AVCodec* codec_ = nullptr;
  AVCodecContext* ctx_ = nullptr;
  AVFrame* frame_ = nullptr;
  AVFrame* sw_frame_ = nullptr;
  AVPacket* pkt_ = nullptr;
  AVFrame* out_frame_ = nullptr;
  SwsContext* sws_ = nullptr;
  AVBufferRef* hw_device_ = nullptr;
  AVPixelFormat hw_pix_fmt_ = AV_PIX_FMT_NONE;
  AVPixelFormat cached_fmt_ = AV_PIX_FMT_NONE;
  int cached_width_ = 0;
  int cached_height_ = 0;
  bool want_bgra_ = true;
  bool is_hw_ = false;
  bool opened_ = false;
  std::deque<DecodedImage> ready_;
};

}  // namespace rc

#endif  // RC_HAVE_FFMPEG
