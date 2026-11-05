#include "codec/encoder.h"
#include "codec/decoder.h"

#ifdef _WIN32
#include "codec/h264_mf_encoder.h"
#include "codec/jpeg_encoder.h"
#include "codec/h264_decoder.h"
#endif

#ifdef __linux__
#include "codec/ffmpeg_h264_encoder.h"
#include "codec/ffmpeg_jpeg_encoder.h"
#include "codec/ffmpeg_h264_decoder.h"
#endif

namespace rc {

std::unique_ptr<IVideoEncoder> create_encoder(const std::string& kind,
                                              std::string* error) {
#ifdef _WIN32
  if (kind == "jpeg") {
    return std::make_unique<JpegEncoder>();
  }
  if (kind == "h264") {
    return std::make_unique<H264MfEncoder>();
  }
#endif

#ifdef __linux__
  if (kind == "jpeg") {
    return std::make_unique<FFmpegJpegEncoder>();
  }
  if (kind == "h264") {
    return std::make_unique<FFmpegH264Encoder>();
  }
#endif

  if (error) *error = "encoder not supported: " + kind;
  return nullptr;
}

std::unique_ptr<IVideoDecoder> create_decoder(std::string* error) {
#ifdef _WIN32
  return std::make_unique<H264Decoder>();
#endif
#ifdef __linux__
  return std::make_unique<H264Decoder>();
#endif
  if (error) *error = "decoder not supported on this platform";
  return nullptr;
}

const char* codec_name(VideoCodec codec) {
  switch (codec) {
    case VideoCodec::H264: return "h264";
    case VideoCodec::H265: return "h265";
    case VideoCodec::AV1: return "av1";
    case VideoCodec::Jpeg: return "jpeg";
  }
  return "unknown";
}

}  // namespace rc
