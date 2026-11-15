#pragma once

#ifdef __linux__

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "codec/encoder.h"

namespace rc {

class FFmpegH264Encoder : public IVideoEncoder {
 public:
  FFmpegH264Encoder();
  ~FFmpegH264Encoder() override;

  bool open(int width, int height, int fps, int quality, std::string* error) override;
  void close() override;
  bool encode(const CapturedFrame& source, EncodedFrame& out, std::string* error) override;
  bool request_keyframe() override;
  const char* name() const override {
    return is_hw_ ? "h264-ffmpeg-hw" : "h264-ffmpeg";
  }

 private:
  bool finish_open();

  const AVCodec* codec_ = nullptr;
  AVCodecContext* ctx_ = nullptr;
  AVFrame* frame_ = nullptr;
  AVPacket* pkt_ = nullptr;
  SwsContext* sws_ = nullptr;
  AVPixelFormat rc_pix_fmt_ = AV_PIX_FMT_NONE;
  int width_ = 0;
  int height_ = 0;
  int fps_ = 30;
  int quality_ = 75;
  int crf_ = 23;
  int64_t pts_ = 0;
  std::atomic<bool> keyframe_requested_{false};
  bool is_hw_ = false;
  bool opened_ = false;
  std::deque<uint64_t> pending_capture_ts_;
};

}  // namespace rc

#endif  // __linux__
