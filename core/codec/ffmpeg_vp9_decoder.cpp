#include "codec/ffmpeg_vp9_decoder.h"

#if defined(RC_HAVE_FFMPEG) && RC_HAVE_FFMPEG

#include <cstring>

#include "base/log.h"

namespace rc {
namespace {

// get_format 只在硬件格式确实在候选列表里时才选择它，否则交回软件格式，
// 由解码器自行走软解路径，避免硬解初始化失败导致整条链路不可用。
AVPixelFormat pick_format(AVCodecContext* ctx, const AVPixelFormat* formats) {
  const AVPixelFormat wanted = *static_cast<AVPixelFormat*>(ctx->opaque);
  for (const AVPixelFormat* p = formats; *p != AV_PIX_FMT_NONE; ++p) {
    if (*p == wanted) return wanted;
  }
  return formats[0];
}

bool supports_hw_device(const AVCodec* codec, AVHWDeviceType type) {
  for (int i = 0;; ++i) {
    const AVCodecHWConfig* config = avcodec_get_hw_config(codec, i);
    if (config == nullptr) break;
    if (config->device_type == type &&
        (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {
      return true;
    }
  }
  return false;
}

}  // namespace

FFmpegVp9Decoder::FFmpegVp9Decoder() = default;

FFmpegVp9Decoder::~FFmpegVp9Decoder() { close(); }

bool FFmpegVp9Decoder::open(std::string* error) {
  if (opened_) return true;

  const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_VP9);
  if (!codec) {
    if (error) *error = "vp9 decoder not found";
    return false;
  }
  codec_ = codec;

  struct Candidate {
    AVHWDeviceType device;
    AVPixelFormat format;
  };
#ifdef _WIN32
  static const Candidate kCandidates[] = {
      {AV_HWDEVICE_TYPE_D3D11VA, AV_PIX_FMT_D3D11},
      {AV_HWDEVICE_TYPE_CUDA, AV_PIX_FMT_CUDA},
  };
#else
  static const Candidate kCandidates[] = {
      {AV_HWDEVICE_TYPE_VAAPI, AV_PIX_FMT_VAAPI},
      {AV_HWDEVICE_TYPE_CUDA, AV_PIX_FMT_CUDA},
  };
#endif

  for (const Candidate& candidate : kCandidates) {
    if (!supports_hw_device(codec, candidate.device)) continue;
    if (try_hardware(candidate.device, candidate.format, error)) return true;
  }
  return open_software(error);
}

bool FFmpegVp9Decoder::try_hardware(AVHWDeviceType device_type,
                                    AVPixelFormat hw_format,
                                    std::string* error) {
  AVBufferRef* device = nullptr;
  if (av_hwdevice_ctx_create(&device, device_type, nullptr, nullptr, 0) < 0) {
    return false;
  }

  AVCodecContext* ctx = avcodec_alloc_context3(codec_);
  if (!ctx) {
    av_buffer_unref(&device);
    return false;
  }
  hw_pix_fmt_ = hw_format;
  ctx->opaque = &hw_pix_fmt_;
  ctx->get_format = pick_format;
  ctx->hw_device_ctx = av_buffer_ref(device);
  // 远控不能容忍解码器攒帧重排。
  ctx->flags2 |= AV_CODEC_FLAG2_FAST;

  if (avcodec_open2(ctx, codec_, nullptr) < 0) {
    avcodec_free_context(&ctx);
    av_buffer_unref(&device);
    return false;
  }

  ctx_ = ctx;
  hw_device_ = device;
  is_hw_ = true;
  frame_ = av_frame_alloc();
  sw_frame_ = av_frame_alloc();
  pkt_ = av_packet_alloc();
  if (!frame_ || !sw_frame_ || !pkt_) {
    release_decoder();
    if (error) *error = "vp9 hw decoder allocation failed";
    return false;
  }
  opened_ = true;
  RC_LOGI("codec", "vp9 decoder hw=" << av_hwdevice_get_type_name(device_type));
  return true;
}

bool FFmpegVp9Decoder::open_software(std::string* error) {
  AVCodecContext* ctx = avcodec_alloc_context3(codec_);
  if (!ctx) {
    if (error) *error = "avcodec_alloc_context3 failed";
    return false;
  }
  ctx->flags2 |= AV_CODEC_FLAG2_FAST;
  if (avcodec_open2(ctx, codec_, nullptr) < 0) {
    if (error) *error = "avcodec_open2 failed for vp9 software decoder";
    avcodec_free_context(&ctx);
    return false;
  }
  ctx_ = ctx;
  is_hw_ = false;
  hw_pix_fmt_ = AV_PIX_FMT_NONE;
  frame_ = av_frame_alloc();
  sw_frame_ = av_frame_alloc();
  pkt_ = av_packet_alloc();
  if (!frame_ || !sw_frame_ || !pkt_) {
    release_decoder();
    if (error) *error = "vp9 software decoder allocation failed";
    return false;
  }
  opened_ = true;
  RC_LOGI("codec", "vp9 decoder software");
  return true;
}

void FFmpegVp9Decoder::release_decoder() {
  if (out_frame_) { av_frame_free(&out_frame_); out_frame_ = nullptr; }
  if (sw_frame_) { av_frame_free(&sw_frame_); sw_frame_ = nullptr; }
  if (frame_) { av_frame_free(&frame_); frame_ = nullptr; }
  if (pkt_) { av_packet_free(&pkt_); pkt_ = nullptr; }
  if (ctx_) { avcodec_free_context(&ctx_); ctx_ = nullptr; }
  if (sws_) { sws_freeContext(sws_); sws_ = nullptr; }
  if (hw_device_) av_buffer_unref(&hw_device_);
  cached_width_ = cached_height_ = 0;
  cached_fmt_ = AV_PIX_FMT_NONE;
  ready_.clear();
  opened_ = false;
  is_hw_ = false;
}

void FFmpegVp9Decoder::close() { release_decoder(); }

bool FFmpegVp9Decoder::fallback_to_software(std::string* error) {
  RC_LOGW("codec", "vp9 hardware decode failed, switching to software decoder");
  release_decoder();
  hw_pix_fmt_ = AV_PIX_FMT_NONE;
  return open_software(error);
}

bool FFmpegVp9Decoder::init_sws(int width, int height, AVPixelFormat src_format) {
  if (width == cached_width_ && height == cached_height_ &&
      src_format == cached_fmt_ && sws_) {
    return true;
  }
  if (sws_) { sws_freeContext(sws_); sws_ = nullptr; }
  if (out_frame_) { av_frame_free(&out_frame_); out_frame_ = nullptr; }

  if (want_bgra_) {
    sws_ = sws_getContext(width, height, src_format, width, height,
                          AV_PIX_FMT_BGRA, SWS_FAST_BILINEAR, nullptr, nullptr,
                          nullptr);
    if (!sws_) return false;
    out_frame_ = av_frame_alloc();
    out_frame_->format = AV_PIX_FMT_BGRA;
    out_frame_->width = width;
    out_frame_->height = height;
    if (av_frame_get_buffer(out_frame_, 0) < 0) return false;
    av_frame_make_writable(out_frame_);
  }
  cached_width_ = width;
  cached_height_ = height;
  cached_fmt_ = src_format;
  return true;
}

bool FFmpegVp9Decoder::convert_frame(AVFrame* frame, int64_t timestamp_us,
                                     DecodedImage& out, std::string* error) {
  if (!init_sws(frame->width, frame->height,
                static_cast<AVPixelFormat>(frame->format))) {
    if (error) *error = "sws init failed";
    return false;
  }

  const int w = frame->width;
  const int h = frame->height;

  if (want_bgra_) {
    sws_scale(sws_, frame->data, frame->linesize, 0, h, out_frame_->data,
              out_frame_->linesize);
    out.width = w;
    out.height = h;
    out.stride = out_frame_->linesize[0];
    out.nv12 = false;
    out.timestamp_us = static_cast<uint64_t>(timestamp_us);
    out.pixels.assign(out_frame_->data[0],
                      out_frame_->data[0] + out.stride * h);
    return true;
  }

  out.width = w;
  out.height = h;
  out.coded_height = h;
  out.stride = w;
  out.nv12 = true;
  out.timestamp_us = static_cast<uint64_t>(timestamp_us);

  const int y_size = w * h;
  const int uv_half_w = w / 2;
  const int uv_half_h = h / 2;
  out.pixels.resize(y_size + uv_half_w * uv_half_h * 2);

  if (frame->format == AV_PIX_FMT_NV12) {
    for (int y = 0; y < h; ++y) {
      std::memcpy(out.pixels.data() + y * w, frame->data[0] + y * frame->linesize[0], w);
    }
    uint8_t* uv_dst = out.pixels.data() + y_size;
    for (int y = 0; y < uv_half_h; ++y) {
      std::memcpy(uv_dst + y * uv_half_w * 2,
                  frame->data[1] + y * frame->linesize[1], uv_half_w * 2);
    }
    return true;
  }

  // YUV420P：Y 平面逐行拷贝，U/V 分立平面交错成 NV12。
  for (int y = 0; y < h; ++y) {
    std::memcpy(out.pixels.data() + y * w, frame->data[0] + y * frame->linesize[0], w);
  }
  uint8_t* uv_dst = out.pixels.data() + y_size;
  const uint8_t* u_src = frame->data[1];
  const uint8_t* v_src = frame->data[2];
  for (int y = 0; y < uv_half_h; ++y) {
    for (int x = 0; x < uv_half_w; ++x) {
      uv_dst[(y * uv_half_w + x) * 2 + 0] = u_src[y * frame->linesize[1] + x];
      uv_dst[(y * uv_half_w + x) * 2 + 1] = v_src[y * frame->linesize[2] + x];
    }
  }
  return true;
}

bool FFmpegVp9Decoder::decode(const uint8_t* data, std::size_t len,
                              int64_t timestamp_us, DecodedImage& out,
                              std::string* error) {
  if (!opened_) {
    if (error) *error = "decoder not open";
    return false;
  }
  if (!ready_.empty()) {
    out = std::move(ready_.front());
    ready_.pop_front();
    return true;
  }

  av_packet_unref(pkt_);
  pkt_->data = const_cast<uint8_t*>(data);
  pkt_->size = static_cast<int>(len);
  pkt_->pts = timestamp_us;
  pkt_->flags = 0;

  int ret = avcodec_send_packet(ctx_, pkt_);
  if (ret < 0) {
    if (error) *error = "avcodec_send_packet failed";
    return false;
  }

  while (true) {
    ret = avcodec_receive_frame(ctx_, frame_);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
    if (ret < 0) {
      if (error) *error = "avcodec_receive_frame failed";
      return false;
    }

    AVFrame* source = frame_;
    if (frame_->hw_frames_ctx != nullptr) {
      av_frame_unref(sw_frame_);
      if (av_hwframe_transfer_data(sw_frame_, frame_, 0) < 0) {
        fallback_to_software(error);
        return false;
      }
      source = sw_frame_;
    }

    DecodedImage picture;
    if (!convert_frame(source, timestamp_us, picture, error)) return false;
    ready_.push_back(std::move(picture));
  }

  if (ready_.empty()) return false;
  out = std::move(ready_.front());
  ready_.pop_front();
  return true;
}

}  // namespace rc

#endif  // RC_HAVE_FFMPEG
