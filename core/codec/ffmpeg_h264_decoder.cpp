#include "codec/ffmpeg_h264_decoder.h"

#include <chrono>
#include <cstring>

#include "base/log.h"

namespace rc {

H264Decoder::H264Decoder() = default;

H264Decoder::~H264Decoder() { close(); }

bool H264Decoder::open(std::string* error) {
  if (opened_) return true;

  codec_ = avcodec_find_decoder(AV_CODEC_ID_H264);
  if (!codec_) {
    if (error) *error = "H.264 decoder not found";
    return false;
  }

  ctx_ = avcodec_alloc_context3(codec_);
  if (!ctx_) {
    if (error) *error = "avcodec_alloc_context3 failed";
    return false;
  }

  if (avcodec_open2(ctx_, codec_, nullptr) < 0) {
    if (error) *error = "avcodec_open2 failed";
    avcodec_free_context(&ctx_);
    return false;
  }

  frame_ = av_frame_alloc();
  pkt_ = av_packet_alloc();
  opened_ = true;
  return true;
}

void H264Decoder::close() {
  if (out_frame_) { av_frame_free(&out_frame_); out_frame_ = nullptr; }
  if (frame_) { av_frame_free(&frame_); frame_ = nullptr; }
  if (pkt_) { av_packet_free(&pkt_); pkt_ = nullptr; }
  if (ctx_) { avcodec_free_context(&ctx_); ctx_ = nullptr; }
  if (sws_) { sws_freeContext(sws_); sws_ = nullptr; }
  cached_width_ = cached_height_ = 0;
  opened_ = false;
}

bool H264Decoder::init_sws(int width, int height) {
  if (width == cached_width_ && height == cached_height_ && sws_) return true;

  if (sws_) { sws_freeContext(sws_); sws_ = nullptr; }
  if (out_frame_) { av_frame_free(&out_frame_); out_frame_ = nullptr; }

  if (want_bgra_) {
    sws_ = sws_getContext(width, height, AV_PIX_FMT_YUV420P,
                          width, height, AV_PIX_FMT_BGRA,
                          SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
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
  return true;
}

bool H264Decoder::convert_frame(AVFrame* frame, int64_t timestamp_us,
                                 DecodedImage& out, std::string* error) {
  if (!init_sws(frame->width, frame->height)) {
    if (error) *error = "sws init failed";
    return false;
  }

  int w = frame->width;
  int h = frame->height;

  if (want_bgra_) {
    sws_scale(sws_,
              frame->data, frame->linesize, 0, h,
              out_frame_->data, out_frame_->linesize);

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

  // NV12 = Y 平面（紧凑）+ 交错 UV 平面（U0 V0 U1 V1 ...）。
  // AV_PIX_FMT_YUV420P 的 data[1]/data[2] 是分立的 U/V 平面，需要交错。
  const int y_size = w * h;
  const int uv_half_w = w / 2;
  const int uv_half_h = h / 2;
  out.pixels.resize(y_size + uv_half_w * uv_half_h * 2);

  // Y 平面：按行拷贝以兼容 linesize[0] > w 的对齐情况。
  for (int y = 0; y < h; ++y) {
    std::memcpy(out.pixels.data() + y * w,
                frame->data[0] + y * frame->linesize[0], w);
  }

  // 交错 UV 平面。
  uint8_t* uv_dst = out.pixels.data() + y_size;
  const uint8_t* u_src = frame->data[1];
  const uint8_t* v_src = frame->data[2];
  for (int y = 0; y < uv_half_h; ++y) {
    for (int x = 0; x < uv_half_w; ++x) {
      uv_dst[(y * uv_half_w + x) * 2 + 0] =
          u_src[y * frame->linesize[1] + x];
      uv_dst[(y * uv_half_w + x) * 2 + 1] =
          v_src[y * frame->linesize[2] + x];
    }
  }
  return true;
}

bool H264Decoder::decode(const uint8_t* data, std::size_t len, int64_t timestamp_us,
                         DecodedImage& out, std::string* error) {
  if (!opened_) {
    if (error) *error = "decoder not open";
    return false;
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

  ret = avcodec_receive_frame(ctx_, frame_);
  if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
    return false;
  }
  if (ret < 0) {
    if (error) *error = "avcodec_receive_frame failed";
    return false;
  }

  return convert_frame(frame_, timestamp_us, out, error);
}

}  // namespace rc
