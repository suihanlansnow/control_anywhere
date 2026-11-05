#include "codec/ffmpeg_h264_encoder.h"

#include <cstdlib>
#include <cstring>

#include "base/clock.h"
#include "base/log.h"

namespace rc {

struct HwCodecProfile {
  const char* name;
  AVPixelFormat pix_fmt;
};

static const HwCodecProfile kHwProfiles[] = {
    {"h264_nvenc", AV_PIX_FMT_NV12},
    {"h264_qsv",   AV_PIX_FMT_NV12},
    {nullptr, AV_PIX_FMT_NONE}};

namespace {

inline int quality_to_crf(int q) {
  if (q <= 0) q = 75;
  if (q > 100) q = 100;
  return 18 + (100 - q) * 22 / 100;
}

// 硬件编码器优先级由用户运行时指定：环境变量 RC_H264_ENCODER=auto|nvenc|qsv|soft。
// auto 保持 nvenc > qsv > libx264 的原顺序；显式指定时只尝试该后端，
// 打不开就回退软编，同时把实际选用结果打到日志，便于现场确认。
enum class EncoderPreference { kAuto, kNvenc, kQsv, kSoft };

EncoderPreference read_encoder_preference() {
  const char* value = std::getenv("RC_H264_ENCODER");
  if (value == nullptr || *value == '\0' || std::strcmp(value, "auto") == 0) {
    return EncoderPreference::kAuto;
  }
  if (std::strcmp(value, "nvenc") == 0) return EncoderPreference::kNvenc;
  if (std::strcmp(value, "qsv") == 0) return EncoderPreference::kQsv;
  if (std::strcmp(value, "soft") == 0 || std::strcmp(value, "libx264") == 0) {
    return EncoderPreference::kSoft;
  }
  RC_LOGW("codec", "unknown RC_H264_ENCODER=" << value << ", falling back to auto");
  return EncoderPreference::kAuto;
}

}  // namespace

FFmpegH264Encoder::FFmpegH264Encoder() = default;

FFmpegH264Encoder::~FFmpegH264Encoder() { close(); }

bool FFmpegH264Encoder::open(int width, int height, int fps, int quality,
                             std::string* error) {
  if (opened_) return true;
  width_ = width;
  height_ = height;
  fps_ = fps > 0 ? fps : 30;
  quality_ = quality > 0 ? quality : 75;
  crf_ = quality_to_crf(quality_);

  const EncoderPreference preference = read_encoder_preference();

  for (const HwCodecProfile* p = kHwProfiles;
       p->name != nullptr && preference != EncoderPreference::kSoft; ++p) {
    if (preference == EncoderPreference::kNvenc && std::strcmp(p->name, "h264_nvenc") != 0) continue;
    if (preference == EncoderPreference::kQsv && std::strcmp(p->name, "h264_qsv") != 0) continue;

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
      char cqp[16];
      std::snprintf(cqp, sizeof(cqp), "%d", crf_);
      av_dict_set(&opts, "cqp", cqp, 0);
    } else if (!std::strcmp(p->name, "h264_qsv")) {
      av_dict_set(&opts, "preset", "medium", 0);
      av_dict_set(&opts, "rate_control", "CQP", 0);
      char cqp[16];
      std::snprintf(cqp, sizeof(cqp), "%d %d %d", crf_, crf_, crf_);
      av_dict_set(&opts, "qp", cqp, 0);
    }

    if (avcodec_open2(ctx_, codec_, &opts) == 0) {
      av_dict_free(&opts);
      is_hw_ = true;
      rc_pix_fmt_ = p->pix_fmt;
      RC_LOGI("codec", "ffmpeg h264 encoder=" << p->name << " (hardware)");
      return finish_open();
    }
    RC_LOGW("codec", "hw encoder " << p->name << " open failed, trying next");
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
  std::snprintf(crf, sizeof(crf), "%d", crf_);
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
  RC_LOGI("codec", "ffmpeg h264 encoder=" << codec_->name << " (software)");

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
  // 时间戳队列只为把输出的包映射回采集时刻；编码器长时间不出包时不能无限增长，
  // 只保留最近若干帧即可（更老的映射已无意义）。
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
    // 一个输入帧可能产出多个包（B 帧/异步编码），队列空了就退回当前时间，
    // 避免对空 deque 取 front/pop_front 造成未定义行为。
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
