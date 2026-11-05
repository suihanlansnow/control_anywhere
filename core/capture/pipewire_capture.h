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
#include <pipewire/stream.h>
#include <pipewire/core.h>
#include <pipewire/context.h>
#include <pipewire/thread-loop.h>
#include <spa/param/video/raw.h>
#include <spa/param/video/format-utils.h>
#include <spa/utils/hook.h>
#include <gio/gio.h>
#endif

struct pw_context;
struct pw_core;
struct pw_stream;
struct pw_thread_loop;
struct spa_buffer;

namespace rc {

class PipewireCapture : public ICaptureSource {
 public:
  PipewireCapture();
  ~PipewireCapture() override;

  friend void pw_state_changed_cb(void*, enum pw_stream_state,
                                 enum pw_stream_state, const char*);
  friend void pw_process_cb(void*);
  friend void pw_param_changed_cb(void*, uint32_t, const struct spa_pod*);
  friend void portal_on_response(GDBusConnection*, const char*, const char*,
                                 const char*, const char*, GVariant*, void*);

  bool start(int monitor_index, std::string* error) override;
  void stop() override;
  bool grab(CapturedFrame& frame, int timeout_ms) override;

  bool set_output_format(PixelFormat format) override {
    return format == PixelFormat::kBGRA;
  }
  const char* name() const override { return "pipewire-screencast"; }

 private:
  bool run_portal_pick(std::string* error);
  bool init_pipewire(uint32_t target_node_id, std::string* error);
  void destroy_pipewire();
  void gmain_thread_fn();

  pw_context* pw_ctx_ = nullptr;
  pw_core* pw_core_ = nullptr;
  pw_stream* pw_stream_ = nullptr;
  pw_thread_loop* pw_thread_loop_ = nullptr;
  struct spa_hook stream_listener_ = {};
  struct spa_video_info_raw video_info_ = {};
  std::atomic<bool> stream_connected_{false};

  std::mutex frame_mutex_;
  std::condition_variable frame_cv_;
  std::vector<uint8_t> latest_pixels_;
  int latest_width_ = 0;
  int latest_height_ = 0;
  int latest_stride_ = 0;
  int64_t latest_ts_us_ = 0;

  std::condition_variable portal_cv_;
  std::thread gmain_thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> gmain_ready_{false};
  std::atomic<bool> portal_done_{false};
  std::atomic<bool> portal_ok_{false};
  std::atomic<uint32_t> portal_node_id_{0};
};

}  // namespace rc
