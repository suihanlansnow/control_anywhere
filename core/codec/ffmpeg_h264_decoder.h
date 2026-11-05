#pragma once

#ifdef __linux__

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "codec/decoder.h"

namespace rc {

class H264Decoder : public IVideoDecoder {
 public:
  H264Decoder();
  ~H264Decoder() override;

  void set_output(Output output) override { want_bgra_ = output == Output::kBgra; }

  bool open(std::string* error) override;
  void close() override;
  const char* name() const override {
    return want_bgra_ ? "h264-ffmpeg-bgra" : "h264-ffmpeg-nv12";
  }

  bool decode(const uint8_t* data, std::size_t len, int64_t timestamp_us,
              DecodedImage& out, std::string* error) override;

 private:
  bool init_sws(int width, int height);
  bool convert_frame(AVFrame* frame, int64_t timestamp_us, DecodedImage& out,
                     std::string* error);

  const AVCodec* codec_ = nullptr;
  AVCodecContext* ctx_ = nullptr;
  AVFrame* frame_ = nullptr;
  AVPacket* pkt_ = nullptr;
  AVFrame* out_frame_ = nullptr;
  SwsContext* sws_ = nullptr;
  int cached_width_ = 0;
  int cached_height_ = 0;
  bool want_bgra_ = true;
  bool opened_ = false;
};

}  // namespace rc

#endif  // __linux__
