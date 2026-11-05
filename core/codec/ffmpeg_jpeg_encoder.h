#pragma once

#ifdef __linux__

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <memory>
#include <string>
#include <vector>

#include "codec/encoder.h"

namespace rc {

class FFmpegJpegEncoder : public IVideoEncoder {
 public:
  FFmpegJpegEncoder();
  ~FFmpegJpegEncoder() override;

  bool open(int width, int height, int fps, int quality, std::string* error) override;
  void close() override;
  bool encode(const CapturedFrame& source, EncodedFrame& out, std::string* error) override;
  const char* name() const override { return "jpeg-ffmpeg"; }

 private:
  const AVCodec* codec_ = nullptr;
  AVCodecContext* ctx_ = nullptr;
  AVFrame* frame_ = nullptr;
  AVPacket* pkt_ = nullptr;
  SwsContext* sws_ = nullptr;
  AVFrame* bgra_frame_ = nullptr;
  int width_ = 0;
  int height_ = 0;
  int quality_ = 75;
  int64_t pts_ = 0;
  bool opened_ = false;
};

}  // namespace rc

#endif  // __linux__
