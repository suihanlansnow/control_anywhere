#include "codec/ffmpeg_jpeg_encoder.h"

#include "base/clock.h"

namespace rc {

FFmpegJpegEncoder::FFmpegJpegEncoder() = default;

FFmpegJpegEncoder::~FFmpegJpegEncoder() { close(); }

bool FFmpegJpegEncoder::open(int width, int height, int fps, int quality,
                             std::string* error) {
  if (opened_) return true;
  width_ = width;
  height_ = height;
  quality_ = quality > 0 ? quality : 75;

  codec_ = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
  if (!codec_) {
    if (error) *error = "MJPEG encoder not found";
    return false;
  }

  ctx_ = avcodec_alloc_context3(codec_);
  if (!ctx_) {
    if (error) *error = "avcodec_alloc_context3 failed";
    return false;
  }

  ctx_->width = width;
  ctx_->height = height;
  ctx_->time_base = {1, std::max(fps, 1)};
  ctx_->framerate = {std::max(fps, 1), 1};
  ctx_->pix_fmt = AV_PIX_FMT_YUVJ420P;
  ctx_->qmin = 2;
  ctx_->qmax = 31;
  ctx_->global_quality = 32 + (100 - quality_) * 32 / 100;

  AVDictionary* opts = nullptr;
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%d", quality_);
  av_dict_set(&opts, "q:v", buf, 0);

  if (avcodec_open2(ctx_, codec_, &opts) < 0) {
    if (error) *error = "avcodec_open2 failed for MJPEG";
    av_dict_free(&opts);
    avcodec_free_context(&ctx_);
    return false;
  }
  av_dict_free(&opts);

  frame_ = av_frame_alloc();
  frame_->format = AV_PIX_FMT_YUVJ420P;
  frame_->width = width;
  frame_->height = height;
  if (av_frame_get_buffer(frame_, 0) < 0) {
    if (error) *error = "av_frame_get_buffer failed";
    avcodec_free_context(&ctx_);
    av_frame_free(&frame_);
    return false;
  }
  av_frame_make_writable(frame_);

  sws_ = sws_getContext(width, height, AV_PIX_FMT_BGRA,
                        width, height, AV_PIX_FMT_YUVJ420P,
                        SWS_BILINEAR, nullptr, nullptr, nullptr);
  if (!sws_) {
    if (error) *error = "sws_getContext failed";
    close();
    return false;
  }

  pkt_ = av_packet_alloc();
  if (!pkt_) {
    if (error) *error = "av_packet_alloc failed";
    close();
    return false;
  }

  opened_ = true;
  return true;
}

void FFmpegJpegEncoder::close() {
  if (pkt_) { av_packet_free(&pkt_); }
  if (frame_) { av_frame_free(&frame_); }
  if (ctx_) { avcodec_free_context(&ctx_); }
  if (sws_) { sws_freeContext(sws_); sws_ = nullptr; }
  opened_ = false;
  pts_ = 0;
}

bool FFmpegJpegEncoder::encode(const CapturedFrame& source, EncodedFrame& out,
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

  int ret = avcodec_send_frame(ctx_, frame_);
  if (ret < 0) {
    if (error) *error = "avcodec_send_frame failed";
    return false;
  }

  av_packet_unref(pkt_);
  ret = avcodec_receive_packet(ctx_, pkt_);
  if (ret < 0) {
    if (error) *error = "avcodec_receive_packet failed";
    return false;
  }

  out.codec = VideoCodec::Jpeg;
  out.keyframe = (pkt_->flags & AV_PKT_FLAG_KEY) != 0;
  out.width = source.width;
  out.height = source.height;
  out.capture_ts_us = source.capture_ts_us;
  out.encode_ts_us = static_cast<uint64_t>(now_us());
  out.data.assign(pkt_->data, pkt_->data + pkt_->size);

  return true;
}

}  // namespace rc
