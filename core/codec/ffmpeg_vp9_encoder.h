#pragma once

// VP9 编码：优先尝试平台硬件编码器（QSV / VAAPI），全部失败后回退 libvpx-vp9 软编。
// Windows 系统本身通常不提供 VP9 编码 MFT，因此软编依赖随包分发的 FFmpeg。
#if defined(RC_HAVE_FFMPEG) && RC_HAVE_FFMPEG

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <atomic>
#include <cstdint>
#include <deque>
#include <string>

#include "codec/encoder.h"

namespace rc {

class FFmpegVp9Encoder : public IVideoEncoder {
 public:
  FFmpegVp9Encoder();
  ~FFmpegVp9Encoder() override;

  bool open(int width, int height, int fps, int quality, std::string* error) override;
  void close() override;
  bool encode(const CapturedFrame& source, EncodedFrame& out,
              std::string* error) override;
  bool request_keyframe() override;
  const char* name() const override { return backend_.c_str(); }
  bool is_hardware() const override { return is_hw_; }

 private:
  // 尝试打开单个后端；成功返回 true。
  bool try_backend(const char* encoder_name, AVHWDeviceType device_type,
                   bool need_upload, std::string* error);
  bool finish_open(std::string* error);
  bool open_software(std::string* error);

  const AVCodec* codec_ = nullptr;
  AVCodecContext* ctx_ = nullptr;
  AVFrame* frame_ = nullptr;     // 送给编码器的帧（软件格式或硬件帧）
  AVFrame* sw_frame_ = nullptr;  // VAAPI 上传前的中转帧（NV12）
  AVPacket* pkt_ = nullptr;
  SwsContext* sws_ = nullptr;
  AVBufferRef* hw_device_ = nullptr;
  AVBufferRef* hw_frames_ = nullptr;
  AVPixelFormat sw_pix_fmt_ = AV_PIX_FMT_NONE;
  bool upload_hw_ = false;
  bool is_hw_ = false;
  bool opened_ = false;
  int width_ = 0;
  int height_ = 0;
  int fps_ = 30;
  int quality_ = 75;
  int crf_ = 23;
  int64_t pts_ = 0;
  std::string backend_ = "vp9-ffmpeg";
  std::atomic<bool> keyframe_requested_{false};
  std::deque<uint64_t> pending_capture_ts_;
};

}  // namespace rc

#endif  // RC_HAVE_FFMPEG
