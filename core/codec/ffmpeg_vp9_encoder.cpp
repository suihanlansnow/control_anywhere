#include "codec/ffmpeg_vp9_encoder.h"

#if defined(RC_HAVE_FFMPEG) && RC_HAVE_FFMPEG

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "base/clock.h"
#include "base/log.h"

namespace rc {
namespace {

inline int quality_to_crf(int q) {
  if (q <= 0) q = 75;
  if (q > 100) q = 100;
  return 18 + (100 - q) * 22 / 100;
}

enum class EncoderPreference { kAuto, kQsv, kVaaapi, kSoft };

// 现场排障用：RC_VP9_ENCODER=auto|qsv|vaapi|soft，默认 auto（硬件优先）。
EncoderPreference read_preference() {
  const char* value = std::getenv("RC_VP9_ENCODER");
  if (value == nullptr || *value == '\0' || std::strcmp(value, "auto") == 0) {
    return EncoderPreference::kAuto;
  }
  if (std::strcmp(value, "qsv") == 0) return EncoderPreference::kQsv;
  if (std::strcmp(value, "vaapi") == 0) return EncoderPreference::kVaaapi;
  if (std::strcmp(value, "soft") == 0 || std::strcmp(value, "libvpx") == 0) {
    return EncoderPreference::kSoft;
  }
  RC_LOGW("codec", "unknown RC_VP9_ENCODER=" << value << ", falling back to auto");
  return EncoderPreference::kAuto;
}

}  // namespace

FFmpegVp9Encoder::FFmpegVp9Encoder() = default;

FFmpegVp9Encoder::~FFmpegVp9Encoder() { close(); }

bool FFmpegVp9Encoder::open(int width, int height, int fps, int quality,
                            std::string* error) {
  if (opened_) return true;
  width_ = width;
  height_ = height;
  fps_ = fps > 0 ? fps : 30;
  quality_ = quality > 0 ? quality : 75;
  crf_ = quality_to_crf(quality_);

  const EncoderPreference preference = read_preference();

  struct Candidate {
    const char* name;
    AVHWDeviceType device;
    bool need_upload;
    EncoderPreference tag;
  };
  // 硬件候选逐个真打开，任何一个失败都继续尝试下一个，最后才落到 libvpx 软编。
  static const Candidate kCandidates[] = {
      {"vp9_qsv", AV_HWDEVICE_TYPE_QSV, false, EncoderPreference::kQsv},
#ifdef __linux__
      {"vp9_vaapi", AV_HWDEVICE_TYPE_VAAPI, true, EncoderPreference::kVaaapi},
#endif
  };

  if (preference != EncoderPreference::kSoft) {
    for (const Candidate& candidate : kCandidates) {
      if (preference != EncoderPreference::kAuto && preference != candidate.tag) continue;
      if (try_backend(candidate.name, candidate.device, candidate.need_upload, error)) {
        return true;
      }
    }
  }

  if (preference == EncoderPreference::kQsv ||
      preference == EncoderPreference::kVaaapi) {
    RC_LOGW("codec", "requested vp9 hardware encoder unavailable, falling back to software");
  }
  return open_software(error);
}

bool FFmpegVp9Encoder::try_backend(const char* encoder_name,
                                   AVHWDeviceType device_type, bool need_upload,
                                   std::string* error) {
  const AVCodec* codec = avcodec_find_encoder_by_name(encoder_name);
  if (!codec) return false;

  AVBufferRef* device = nullptr;
  if (av_hwdevice_ctx_create(&device, device_type, nullptr, nullptr, 0) < 0) {
    RC_LOGW("codec", "vp9 hw device " << encoder_name << " unavailable");
    return false;
  }

  AVCodecContext* ctx = avcodec_alloc_context3(codec);
  if (!ctx) {
    av_buffer_unref(&device);
    return false;
  }
  ctx->width = width_;
  ctx->height = height_;
  ctx->time_base = {1, fps_};
  ctx->framerate = {fps_, 1};
  ctx->gop_size = fps_;
  ctx->max_b_frames = 0;
  ctx->bit_rate = 0;
  ctx->hw_device_ctx = av_buffer_ref(device);

  AVBufferRef* frames_ref = nullptr;
  if (need_upload) {
    // VAAPI 只接受硬件帧：先建 NV12 的 frames 池，编码前把软件帧上传进去。
    frames_ref = av_hwframe_ctx_alloc(device);
    if (!frames_ref) {
      avcodec_free_context(&ctx);
      av_buffer_unref(&device);
      return false;
    }
    auto* frames = reinterpret_cast<AVHWFramesContext*>(frames_ref->data);
    frames->format = AV_PIX_FMT_VAAPI;
    frames->sw_format = AV_PIX_FMT_NV12;
    frames->width = width_;
    frames->height = height_;
    if (av_hwframe_ctx_init(frames_ref) < 0) {
      av_buffer_unref(&frames_ref);
      avcodec_free_context(&ctx);
      av_buffer_unref(&device);
      return false;
    }
    ctx->pix_fmt = AV_PIX_FMT_VAAPI;
    ctx->hw_frames_ctx = av_buffer_ref(frames_ref);
  } else {
    ctx->pix_fmt = AV_PIX_FMT_NV12;
  }

  AVDictionary* opts = nullptr;
  if (std::strcmp(encoder_name, "vp9_qsv") == 0) {
    av_dict_set(&opts, "preset", "medium", 0);
  }
  if (avcodec_open2(ctx, codec, &opts) < 0) {
    RC_LOGW("codec", "vp9 hw encoder " << encoder_name << " open failed");
    av_dict_free(&opts);
    if (frames_ref) av_buffer_unref(&frames_ref);
    avcodec_free_context(&ctx);
    av_buffer_unref(&device);
    return false;
  }
  av_dict_free(&opts);

  codec_ = codec;
  ctx_ = ctx;
  hw_device_ = device;
  hw_frames_ = frames_ref;
  upload_hw_ = need_upload;
  is_hw_ = true;
  sw_pix_fmt_ = AV_PIX_FMT_NV12;
  backend_ = encoder_name;
  RC_LOGI("codec", "vp9 encoder=" << encoder_name << " (hardware)");
  if (!finish_open(error)) {
    close();
    return false;
  }
  return true;
}

bool FFmpegVp9Encoder::open_software(std::string* error) {
  const AVCodec* codec = avcodec_find_encoder_by_name("libvpx-vp9");
  if (!codec) codec = avcodec_find_encoder(AV_CODEC_ID_VP9);
  if (!codec) {
    if (error) *error = "no vp9 encoder available (libvpx-vp9 missing)";
    return false;
  }

  AVCodecContext* ctx = avcodec_alloc_context3(codec);
  if (!ctx) {
    if (error) *error = "avcodec_alloc_context3 failed";
    return false;
  }
  ctx->width = width_;
  ctx->height = height_;
  ctx->time_base = {1, fps_};
  ctx->framerate = {fps_, 1};
  ctx->gop_size = fps_;
  ctx->max_b_frames = 0;
  ctx->pix_fmt = AV_PIX_FMT_YUV420P;
  ctx->bit_rate = 0;

  AVDictionary* opts = nullptr;
  // 实时远控：禁用前瞻与 alt-ref，避免编码器攒帧引入额外延迟。
  av_dict_set(&opts, "deadline", "realtime", 0);
  av_dict_set(&opts, "cpu-used", "8", 0);
  av_dict_set(&opts, "lag-in-frames", "0", 0);
  av_dict_set(&opts, "auto-alt-ref", "0", 0);
  av_dict_set(&opts, "error-resilient", "1", 0);
  av_dict_set(&opts, "row-mt", "1", 0);
  char crf[16];
  std::snprintf(crf, sizeof(crf), "%d", crf_);
  av_dict_set(&opts, "crf", crf, 0);

  if (avcodec_open2(ctx, codec, &opts) < 0) {
    if (error) *error = std::string("avcodec_open2 failed for ") + codec->name;
    av_dict_free(&opts);
    avcodec_free_context(&ctx);
    return false;
  }
  av_dict_free(&opts);

  codec_ = codec;
  ctx_ = ctx;
  is_hw_ = false;
  upload_hw_ = false;
  sw_pix_fmt_ = AV_PIX_FMT_YUV420P;
  backend_ = codec->name;
  RC_LOGI("codec", "vp9 encoder=" << codec->name << " (software)");
  if (!finish_open(error)) {
    close();
    return false;
  }
  return true;
}

bool FFmpegVp9Encoder::finish_open(std::string* error) {
  if (upload_hw_) {
    sw_frame_ = av_frame_alloc();
    frame_ = av_frame_alloc();
    if (!sw_frame_ || !frame_) {
      if (error) *error = "av_frame_alloc failed";
      return false;
    }
    sw_frame_->format = sw_pix_fmt_;
    sw_frame_->width = width_;
    sw_frame_->height = height_;
    if (av_frame_get_buffer(sw_frame_, 0) < 0) {
      if (error) *error = "sw frame buffer alloc failed";
      return false;
    }
  } else {
    frame_ = av_frame_alloc();
    if (!frame_) {
      if (error) *error = "av_frame_alloc failed";
      return false;
    }
    frame_->format = sw_pix_fmt_;
    frame_->width = width_;
    frame_->height = height_;
    if (av_frame_get_buffer(frame_, 0) < 0) {
      if (error) *error = "frame buffer alloc failed";
      return false;
    }
  }

  // 采集侧统一给 BGRA，编码器按后端要求转 NV12 或 I420。
  sws_ = sws_getContext(width_, height_, AV_PIX_FMT_BGRA, width_, height_,
                        sw_pix_fmt_, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
  if (!sws_) {
    if (error) *error = "sws_getContext failed";
    return false;
  }

  pkt_ = av_packet_alloc();
  if (!pkt_) {
    if (error) *error = "av_packet_alloc failed";
    return false;
  }
  opened_ = true;
  return true;
}

void FFmpegVp9Encoder::close() {
  if (pkt_) av_packet_free(&pkt_);
  if (sw_frame_) av_frame_free(&sw_frame_);
  if (frame_) av_frame_free(&frame_);
  if (ctx_) avcodec_free_context(&ctx_);
  if (sws_) { sws_freeContext(sws_); sws_ = nullptr; }
  if (hw_frames_) av_buffer_unref(&hw_frames_);
  if (hw_device_) av_buffer_unref(&hw_device_);
  pkt_ = nullptr;
  sw_frame_ = nullptr;
  frame_ = nullptr;
  ctx_ = nullptr;
  codec_ = nullptr;
  opened_ = false;
  is_hw_ = false;
  upload_hw_ = false;
  sw_pix_fmt_ = AV_PIX_FMT_NONE;
  pts_ = 0;
  pending_capture_ts_.clear();
}

bool FFmpegVp9Encoder::request_keyframe() {
  if (!opened_) return false;
  keyframe_requested_.store(true);
  return true;
}

bool FFmpegVp9Encoder::encode(const CapturedFrame& source, EncodedFrame& out,
                              std::string* error) {
  if (!opened_) {
    if (error) *error = "encoder not open";
    return false;
  }
  if (source.pixels.empty()) {
    if (error) *error = "no source pixels";
    return false;
  }

  AVFrame* target = upload_hw_ ? sw_frame_ : frame_;
  if (av_frame_make_writable(target) < 0) {
    if (error) *error = "frame not writable";
    return false;
  }

  const uint8_t* src_data[1] = {source.pixels.data()};
  int src_linesize[1] = {source.stride > 0 ? source.stride : source.width * 4};
  sws_scale(sws_, src_data, src_linesize, 0, source.height, target->data,
            target->linesize);

  if (upload_hw_) {
    av_frame_unref(frame_);
    if (av_hwframe_get_buffer(hw_frames_, frame_, 0) < 0) {
      if (error) *error = "hw frame alloc failed";
      return false;
    }
    if (av_hwframe_transfer_data(frame_, sw_frame_, 0) < 0) {
      if (error) *error = "hw frame upload failed";
      return false;
    }
  }

  frame_->pict_type = keyframe_requested_.exchange(false) ? AV_PICTURE_TYPE_I
                                                          : AV_PICTURE_TYPE_NONE;
  frame_->pts = pts_++;

  constexpr std::size_t kMaxPendingTimestamps = 16;
  pending_capture_ts_.push_back(static_cast<uint64_t>(source.capture_ts_us));
  while (pending_capture_ts_.size() > kMaxPendingTimestamps) {
    pending_capture_ts_.pop_front();
  }

  int ret = avcodec_send_frame(ctx_, frame_);
  if (ret < 0) {
    if (error) *error = "avcodec_send_frame failed";
    return false;
  }

  bool got_packet = false;
  while (ret >= 0) {
    av_packet_unref(pkt_);
    ret = avcodec_receive_packet(ctx_, pkt_);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
    if (ret < 0) {
      if (error) *error = "avcodec_receive_packet failed";
      return false;
    }
    got_packet = true;
    out.codec = VideoCodec::Vp9;
    out.keyframe = (pkt_->flags & AV_PKT_FLAG_KEY) != 0;
    out.width = source.width;
    out.height = source.height;
    if (!pending_capture_ts_.empty()) {
      out.capture_ts_us = pending_capture_ts_.front();
      pending_capture_ts_.pop_front();
    } else {
      out.capture_ts_us = static_cast<uint64_t>(now_us());
    }
    out.encode_ts_us = static_cast<uint64_t>(now_us());
    out.data.assign(pkt_->data, pkt_->data + pkt_->size);
  }
  return got_packet;
}

}  // namespace rc

#endif  // RC_HAVE_FFMPEG
