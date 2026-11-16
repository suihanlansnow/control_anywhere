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

#if defined(RC_HAVE_FFMPEG) && RC_HAVE_FFMPEG
#include "codec/ffmpeg_vp9_decoder.h"
#include "codec/ffmpeg_vp9_encoder.h"
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

#if defined(RC_HAVE_FFMPEG) && RC_HAVE_FFMPEG
  if (kind == "vp9") {
    return std::make_unique<FFmpegVp9Encoder>();
  }
#endif

  if (error) *error = "encoder not supported: " + kind;
  return nullptr;
}

std::unique_ptr<IVideoDecoder> create_decoder(const std::string& codec,
                                              std::string* error) {
  if (codec == "h264") {
#ifdef _WIN32
    return std::make_unique<H264Decoder>();
#endif
#ifdef __linux__
    return std::make_unique<H264Decoder>();
#endif
  }

  if (codec == "vp9") {
#if defined(RC_HAVE_FFMPEG) && RC_HAVE_FFMPEG
    return std::make_unique<FFmpegVp9Decoder>();
#else
    if (error) *error = "vp9 decoder not built in this package";
    return nullptr;
#endif
  }

  if (error) *error = "decoder not supported: " + codec;
  return nullptr;
}

const char* codec_name(VideoCodec codec) {
  switch (codec) {
    case VideoCodec::H264: return "h264";
    case VideoCodec::H265: return "h265";
    case VideoCodec::AV1: return "av1";
    case VideoCodec::Jpeg: return "jpeg";
    case VideoCodec::Vp9: return "vp9";
  }
  return "unknown";
}

}  // namespace rc
