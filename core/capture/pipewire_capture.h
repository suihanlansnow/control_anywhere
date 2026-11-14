#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "capture/capture.h"

#if defined(__linux__) && defined(HAVE_PIPEWIRE_CAPTURE)
#include <pipewire/pipewire.h>
#include <pipewire/stream.h>
#include <pipewire/core.h>
#include <pipewire/context.h>
#include <pipewire/thread-loop.h>
#include <spa/param/video/raw.h>
#include <spa/pod/builder.h>
#include <spa/utils/hook.h>
#include <gio/gio.h>
#endif

struct pw_context;
struct pw_core;
struct pw_stream;
struct pw_thread_loop;
struct spa_buffer;
struct _XdpPortal;
struct _XdpSession;

namespace rc {

// 基于 xdg-desktop-portal + PipeWire 的屏幕采集。
// 流程（与 recode_video 一致，跨 GNOME/KDE/wlroots 通用）：
//   1. libportal 创建 screencast session（首次弹窗选屏，后续用 restore token 免弹窗）
//   2. xdp_session_open_pipewire_remote() 拿到 portal 专属的 PipeWire fd
//   3. xdp_session_get_streams() 拿到精确的 node_id
//   4. pw_context_connect_fd() 直连 + pw_stream_connect(node_id) 拉帧
class PipewireCapture : public ICaptureSource {
 public:
  PipewireCapture();
  ~PipewireCapture() override;

  bool start(int monitor_index, std::string* error) override;
  void stop() override;
  bool grab(CapturedFrame& frame, int timeout_ms) override;

  // 当前编码器链路按 BGRA 喂入，NV12 直通暂不支持（返回 false 让上层走 BGRA）。
  bool set_output_format(PixelFormat format) override {
    return format == PixelFormat::kBGRA;
  }
  const char* name() const override { return "pipewire-screencast"; }

 private:
  bool run_portal(int monitor_index, std::string* error);
  bool connect_pipewire(std::string* error);
  bool init_stream(uint32_t node_id, std::string* error);
  void destroy_pipewire();

  // portal
  struct _XdpPortal* portal_ = nullptr;
  struct _XdpSession* session_ = nullptr;
  int pw_remote_fd_ = -1;
  uint32_t target_node_id_ = 0;

  // pipewire
  pw_thread_loop* loop_ = nullptr;
  pw_context* ctx_ = nullptr;
  pw_core* core_ = nullptr;
  pw_stream* stream_ = nullptr;
  struct spa_hook stream_listener_ = {};

  // 协商到的格式
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint32_t stride_ = 0;
  std::string format_;

  // 格式协商同步
  std::mutex fmt_mu_;
  std::condition_variable fmt_cv_;
  bool negotiated_ = false;

  // 帧桥（PipeWire 回调 push -> grab() pull）
  std::mutex frame_mu_;
  std::condition_variable frame_cv_;
  std::vector<uint8_t> latest_pixels_;
  uint64_t latest_ts_us_ = 0;
  bool frame_ready_ = false;

  std::atomic<bool> running_{false};
  std::atomic<bool> stopping_{false};
  std::atomic<bool> stream_gone_{false};

  // PipeWire C 回调
  static void stream_state_changed(void* data, enum pw_stream_state old,
                                   enum pw_stream_state state, const char* error);
  static void stream_param_changed(void* data, uint32_t id,
                                   const struct spa_pod* param);
  static void stream_process(void* data);
};

}  // namespace rc
