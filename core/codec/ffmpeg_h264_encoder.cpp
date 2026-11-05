#include "codec/ffmpeg_h264_encoder.h"

#include <cstring>

#include "base/clock.h"

namespace rc {

struct HwCodecProfile {
  const char* name;
  AVPixelFormat pix_fmt;
};

static const HwCodecProfile kHwProfiles[] = {
    {"h264_nvenc", AV_PIX_FMT_NV12},
    {"h264_qsv",   AV_PIX_FMT_NV12},
    {nullptr, AV_PIX_FMT_NONE}};

FFmpegH264Encoder::FFmpegH264Encoder() = default;

FFmpegH264Encoder::~FFmpegH264Encoder() { close(); }

bool FFmpegH264Encoder::open(int width, int height, int fps, int quality,
                             std::string* error) {
  if (opened_) return true;
  width_ = width;
  height_ = height;
  fps_ = fps > 0 ? fps : 30;
  quality_ = quality > 0 ? quality : 23;

  for (const HwCodecProfile* p = kHwProfiles; p->name; ++p) {
    codec_ = avcodec_find_encoder_by_name(p->name);
    if (!codec_) continue;

    ctx_ = avcodec_alloc_context3(codec_);
    if (!ctx_) continue;

    ctx_->width = width;
    ctx_->height = height;
    ctx_->time_base = {1, fps_};
    ctx_->framerate = {fps_, 1};
    ctx_->pix_fmt = p->pix_fmt;

    AVDictionary* opts = nullptr;
    if (!std::strcmp(p->name, "h264_nvenc")) {
      av_dict_set(&opts, "preset", "p4", 0);
      av_dict_set(&opts, "tune", "ll", 0);
      av_dict_set(&opts, "rc", "vbr", 0);
    } else if (!std::strcmp(p->name, "h264_qsv")) {
      av_dict_set(&opts, "preset", "medium", 0);
      av_dict_set(&opts, "rate_control", "CBR", 0);
    }

    if (avcodec_open2(ctx_, codec_, &opts) == 0) {
      av_dict_free(&opts);
      is_hw_ = true;
      rc_pix_fmt_ = p->pix_fmt;
      return finish_open();
    }
    av_dict_free(&opts);
    avcodec_free_context(&ctx_);
    ctx_ = nullptr;
  }

  codec_ = avcodec_find_encoder(AV_CODEC_ID_H264);
  if (!codec_) {
    if (error) *error = "H264 encoder not found (neither hw nor libx264)";
    return false;
  }
  is_hw_ = false;

  ctx_ = avcodec_alloc_context3(codec_);
  if (!ctx_) {
    if (error) *error = "avcodec_alloc_context3 failed";
    return false;
  }

  ctx_->width = width;
  ctx_->height = height;
  ctx_->time_base = {1, fps_};
  ctx_->framerate = {fps_, 1};
  ctx_->pix_fmt = AV_PIX_FMT_YUV420P;

  AVDictionary* opts = nullptr;
  av_dict_set(&opts, "preset", "veryfast", 0);
  av_dict_set(&opts, "tune", "zerolatency", 0);
  char crf[16];
  std::snprintf(crf, sizeof(crf), "%d", quality_);
  av_dict_set(&opts, "crf", crf, 0);
  av_dict_set(&opts, "profile", "baseline", 0);

  if (avcodec_open2(ctx_, codec_, &opts) < 0) {
    if (error) *error = std::string("avcodec_open2 failed for ") + codec_->name;
    av_dict_free(&opts);
    avcodec_free_context(&ctx_);
    ctx_ = nullptr;
    codec_ = nullptr;
    return false;
  }
  av_dict_free(&opts);
  rc_pix_fmt_ = AV_PIX_FMT_YUV420P;

  return finish_open();
}

bool FFmpegH264Encoder::finish_open() {
  frame_ = av_frame_alloc();
  frame_->format = ctx_->pix_fmt;
  frame_->width = width_;
  frame_->height = height_;
  if (av_frame_get_buffer(frame_, 0) < 0) {
    av_frame_free(&frame_);
    frame_ = nullptr;
    avcodec_free_context(&ctx_);
    ctx_ = nullptr;
    return false;
  }
  av_frame_make_writable(frame_);

  sws_ = sws_getContext(width_, height_, AV_PIX_FMT_BGRA,
                        width_, height_, rc_pix_fmt_,
                        SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
  if (!sws_) {
    avcodec_free_context(&ctx_);
    av_frame_free(&frame_);
    ctx_ = nullptr;
    frame_ = nullptr;
    return false;
  }

  pkt_ = av_packet_alloc();
  if (!pkt_) {
    sws_freeContext(sws_);
    sws_ = nullptr;
    avcodec_free_context(&ctx_);
    ctx_ = nullptr;
    return false;
  }

  opened_ = true;
  return true;
}

void FFmpegH264Encoder::close() {
  if (pkt_) { av_packet_free(&pkt_); }
  if (frame_) { av_frame_free(&frame_); }
  if (ctx_) { avcodec_free_context(&ctx_); }
  if (sws_) { sws_freeContext(sws_); sws_ = nullptr; }
  pkt_ = nullptr;
  frame_ = nullptr;
  ctx_ = nullptr;
  opened_ = false;
  is_hw_ = false;
  pending_capture_ts_.clear();
  pts_ = 0;
  rc_pix_fmt_ = AV_PIX_FMT_NONE;
}

bool FFmpegH264Encoder::encode(const CapturedFrame& source, EncodedFrame& out,
                               std::string* error) {
  if (!opened_) {
    if (error) *error = "encoder not open";
    return false;
  }

  if (source.pixels.empty()) {
    if (error) *error = "no source pixels";
    return false;
  }

  const uint8_t* src_data[1] = {source.pixels.data()};
  int src_linesize[1] = {source.stride > 0 ? source.stride : source.width * 4};

  sws_scale(sws_, src_data, src_linesize, 0, source.height,
            frame_->data, frame_->linesize);

  frame_->pts = pts_++;
  pending_capture_ts_.push_back(static_cast<uint64_t>(source.capture_ts_us));

  int ret = avcodec_send_frame(ctx_, frame_);
  if (ret < 0) {
    if (error) *error = "avcodec_send_frame failed";
    return false;
  }

  bool got_packet = false;
  while (ret >= 0) {
    av_packet_unref(pkt_);
    ret = avcodec_receive_packet(ctx_, pkt_);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
      break;
    }
    if (ret < 0) {
      if (error) *error = "avcodec_receive_packet failed";
      return false;
    }

    got_packet = true;

    out.codec = VideoCodec::H264;
    out.keyframe = (pkt_->flags & AV_PKT_FLAG_KEY) != 0;
    out.width = source.width;
    out.height = source.height;
    out.capture_ts_us = pending_capture_ts_.front();
    pending_capture_ts_.pop_front();
    out.encode_ts_us = static_cast<uint64_t>(now_us());
    out.data.assign(pkt_->data, pkt_->data + pkt_->size);
  }

  return got_packet;
}

}  // namespace rc
