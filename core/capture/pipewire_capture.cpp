#include "capture/pipewire_capture.h"

#include <pipewire/pipewire.h>
#include <pipewire/stream.h>
#include <pipewire/core.h>
#include <pipewire/context.h>
#include <pipewire/thread-loop.h>
#include <pipewire/properties.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/raw.h>
#include <spa/buffer/meta.h>
#include <spa/utils/result.h>

#include <glib.h>
#include <gio/gio.h>

#include <cerrno>
#include <cstring>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

namespace rc {
PipewireCapture::PipewireCapture() = default;
PipewireCapture::~PipewireCapture() { stop(); }

void PipewireCapture::gmain_thread_fn() {
  gmain_ready_.store(true);
  portal_cv_.notify_all();
  GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
  if (!loop) {
    gmain_ready_.store(false);
    return;
  }
  g_main_loop_run(loop);
  g_main_loop_unref(loop);
  gmain_ready_.store(false);
}

bool PipewireCapture::start(int monitor_index, std::string* error) {
  if (running_.load()) return true;
  pw_init(nullptr, nullptr);

  gmain_thread_ = std::thread(&PipewireCapture::gmain_thread_fn, this);
  {
    std::unique_lock<std::mutex> lk(frame_mutex_);
    portal_cv_.wait_for(lk, std::chrono::seconds(5),
                        [this] { return gmain_ready_.load(); });
  }
  if (!gmain_ready_.load()) {
    if (error) *error = "failed to start GMainLoop thread";
    return false;
  }

  uint32_t node_id = 0;
  if (!run_portal_pick(error)) return false;
  node_id = portal_node_id_.load();
  if (node_id == 0) {
    if (error) *error = "screen share portal did not return a node id";
    return false;
  }

  if (!init_pipewire(node_id, error)) return false;

  running_.store(true);
  return true;
}

void PipewireCapture::stop() {
  if (!running_.load() && !gmain_ready_.load()) return;
  running_.store(false);

  destroy_pipewire();

  if (gmain_ready_.load()) {
    GMainContext* ctx = g_main_context_default();
    if (ctx) {
      GMainLoop* loop = g_main_loop_new(ctx, FALSE);
      if (loop) {
        g_main_loop_quit(loop);
        g_main_loop_unref(loop);
      }
    }
  }
  if (gmain_thread_.joinable()) {
    gmain_thread_.join();
  }
  pw_deinit();
}

void portal_on_response(GDBusConnection* conn,
                               const char* sender_name,
                               const char* object_path,
                               const char* interface_name,
                               const char* signal_name,
                               GVariant* parameters,
                               gpointer user_data) {
  (void)conn; (void)sender_name; (void)interface_name; (void)signal_name;
  auto* self = static_cast<PipewireCapture*>(user_data);

  guint32 response = 0;
  GVariant* results = nullptr;
  g_variant_get(parameters, "(u@a{sv})", &response, &results);

  uint32_t node_id = 0;
  if (response == 0 && results) {
    GVariant* streams = g_variant_lookup_value(results, "streams",
                                               G_VARIANT_TYPE("a(ua{sv})"));
    if (streams && g_variant_n_children(streams) > 0) {
      GVariant* first = g_variant_get_child_value(streams, 0);
      g_variant_get_child(first, 0, "u", &node_id);
      g_variant_unref(first);
    }
    if (streams) g_variant_unref(streams);
  }
  if (results) g_variant_unref(results);

  self->portal_node_id_.store(node_id);
  self->portal_ok_.store(response == 0 && node_id != 0);
  self->portal_done_.store(true);
  self->portal_cv_.notify_all();
}

bool PipewireCapture::run_portal_pick(std::string* error) {
  GError* ger = nullptr;
  GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &ger);
  if (!bus) {
    if (error) *error = std::string("D-Bus: ") + ger->message;
    g_error_free(ger);
    return false;
  }

  g_dbus_connection_signal_subscribe(
      bus, "org.freedesktop.portal.Desktop",
      "org.freedesktop.portal.Request", "Response",
      nullptr, nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
      portal_on_response, this, nullptr);

  GVariant* out = g_dbus_connection_call_sync(
      bus, "org.freedesktop.portal.Desktop",
      "/org/freedesktop/portal/desktop",
      "org.freedesktop.portal.ScreenCast", "CreateSession",
      g_variant_new("(a{sv})",
                    "session_handle_token", g_variant_new_string("rc1"),
                    "handle_token", g_variant_new_string("rc2")),
      nullptr, G_DBUS_CALL_FLAGS_NONE, 30000, nullptr, &ger);
  if (!out) {
    if (error) *error = std::string("CreateSession: ") + ger->message;
    g_error_free(ger);
    g_object_unref(bus);
    return false;
  }
  const char* session_path = nullptr;
  g_variant_get_child(out, 0, "o", &session_path);
  g_variant_unref(out);

  out = g_dbus_connection_call_sync(
      bus, "org.freedesktop.portal.Desktop",
      session_path, "org.freedesktop.portal.ScreenCast", "SelectSources",
      g_variant_new("(a{sv})",
                    "multiple", g_variant_new_boolean(FALSE),
                    "types", g_variant_new_uint32(1)),
      nullptr, G_DBUS_CALL_FLAGS_NONE, 30000, nullptr, &ger);
  if (!out) {
    if (error) *error = std::string("SelectSources: ") + ger->message;
    g_error_free(ger);
    g_object_unref(bus);
    return false;
  }
  g_variant_unref(out);

  portal_done_.store(false);
  portal_ok_.store(false);
  portal_node_id_.store(0);

  out = g_dbus_connection_call_sync(
      bus, "org.freedesktop.portal.Desktop",
      session_path, "org.freedesktop.portal.ScreenCast", "Start",
      g_variant_new("(a{sv})", ""),
      nullptr, G_DBUS_CALL_FLAGS_NONE, 30000, nullptr, &ger);
  if (!out) {
    if (error) *error = std::string("Start: ") + ger->message;
    g_error_free(ger);
    g_object_unref(bus);
    return false;
  }
  g_variant_unref(out);

  {
    std::unique_lock<std::mutex> lk(frame_mutex_);
    portal_cv_.wait_for(lk, std::chrono::seconds(120),
                        [this] { return portal_done_.load(); });
  }

  g_object_unref(bus);

  if (!portal_ok_.load()) {
    if (error) *error = "portal rejected or timed out (did user cancel?)";
    return false;
  }
  return true;
}

void pw_state_changed_cb(void* d, enum pw_stream_state old,
                                enum pw_stream_state state, const char* err) {
  (void)old;
  auto* self = static_cast<PipewireCapture*>(d);
  if (state == PW_STREAM_STATE_STREAMING) {
    self->stream_connected_.store(true);
  }
  if ((state == PW_STREAM_STATE_ERROR || state == PW_STREAM_STATE_PAUSED) && err) {
    fprintf(stderr, "[pw] stream state %s: %s\n",
            pw_stream_state_as_string(state), err);
  }
}

void pw_process_cb(void* d) {
  auto* self = static_cast<PipewireCapture*>(d);

  struct pw_buffer* buf = pw_stream_dequeue_buffer(self->pw_stream_);
  if (!buf) return;

  struct spa_buffer* sb = buf->buffer;
  if (!sb || sb->n_datas == 0 || !sb->datas[0].data) {
    pw_stream_queue_buffer(self->pw_stream_, buf);
    return;
  }

  void* ptr = sb->datas[0].data;
  uint32_t size = sb->datas[0].maxsize;

  int64_t ts_us = 0;
  {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ts_us = ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
  }

  int w = self->video_info_.size.width;
  int h = self->video_info_.size.height;
  int stride = w * 4;
  if (stride == 0) stride = w * 4;

  if (w > 0 && h > 0) {
    std::lock_guard<std::mutex> lk(self->frame_mutex_);
    self->latest_width_ = w;
    self->latest_height_ = h;
    self->latest_stride_ = stride;
    self->latest_ts_us_ = ts_us;
    self->latest_pixels_.assign(static_cast<uint8_t*>(ptr),
                                static_cast<uint8_t*>(ptr) + size);
    self->frame_cv_.notify_all();
  }

  pw_stream_queue_buffer(self->pw_stream_, buf);
}

void pw_param_changed_cb(void* d, uint32_t id, const struct spa_pod* param) {
  auto* self = static_cast<PipewireCapture*>(d);
  if (id != SPA_PARAM_Format || !param) return;
  struct spa_video_info_raw info;
  if (spa_format_video_raw_parse(param, &info) == 0) {
    self->video_info_ = info;
  }
}

bool PipewireCapture::init_pipewire(uint32_t target_node_id, std::string* error) {
  pw_ctx_ = pw_context_new(nullptr, nullptr, 0);
  if (!pw_ctx_) { if (error) *error = "pw_context_new failed"; return false; }

  pw_core_ = pw_context_connect(pw_ctx_, nullptr, 0);
  if (!pw_core_) { if (error) *error = "pw_context_connect failed"; destroy_pipewire(); return false; }

  pw_thread_loop_ = pw_thread_loop_new("rc-pw", nullptr);
  if (!pw_thread_loop_) { if (error) *error = "pw_thread_loop_new failed"; destroy_pipewire(); return false; }

  auto* props = pw_properties_new(
      PW_KEY_MEDIA_CLASS, "Video/Source",
      PW_KEY_MEDIA_TYPE, "Video",
      nullptr);

  pw_stream_ = pw_stream_new(pw_core_, "rc-capture", props);
  if (!pw_stream_) { if (error) *error = "pw_stream_new failed"; destroy_pipewire(); return false; }

  struct pw_stream_events ev = {};
  ev.version = PW_VERSION_STREAM_EVENTS;
  ev.state_changed = pw_state_changed_cb;
  ev.process = pw_process_cb;
  ev.param_changed = pw_param_changed_cb;
  pw_stream_add_listener(pw_stream_, &stream_listener_, &ev, this);

  uint8_t format_buf[4096];
  struct spa_pod_builder b;
  spa_pod_builder_init(&b, format_buf, sizeof(format_buf));

  struct spa_video_info_raw video_info;
  std::memset(&video_info, 0, sizeof(video_info));
  video_info.format = SPA_VIDEO_FORMAT_BGRA;
  video_info.size.width = 1920;
  video_info.size.height = 1080;
  video_info.framerate.num = 30;
  video_info.framerate.denom = 1;

  struct spa_pod* fmt = spa_format_video_raw_build(&b, SPA_PARAM_Format, &video_info);

  uint32_t target = target_node_id == 0 ? PW_ID_ANY : target_node_id;

  int r = pw_stream_connect(pw_stream_, PW_DIRECTION_INPUT, target,
                            static_cast<enum pw_stream_flags>(
                                PW_STREAM_FLAG_AUTOCONNECT |
                                PW_STREAM_FLAG_INACTIVE |
                                PW_STREAM_FLAG_MAP_BUFFERS),
                            const_cast<const struct spa_pod**>(&fmt), 1);
  if (r < 0) {
    if (error) *error = std::string("pw_stream_connect: ") + spa_strerror(r);
    destroy_pipewire();
    return false;
  }

  pw_thread_loop_start(pw_thread_loop_);

  pw_thread_loop_lock(pw_thread_loop_);
  pw_stream_set_active(pw_stream_, true);
  pw_thread_loop_unlock(pw_thread_loop_);

  {
    std::unique_lock<std::mutex> lk(frame_mutex_);
    frame_cv_.wait_for(lk, std::chrono::seconds(15),
                       [this] { return stream_connected_.load(); });
  }

  if (!stream_connected_.load()) {
    if (error) *error = "pipewire stream failed to enter STREAMING state";
    destroy_pipewire();
    return false;
  }

  return true;
}

void PipewireCapture::destroy_pipewire() {
  if (pw_thread_loop_) {
    pw_thread_loop_stop(pw_thread_loop_);
    pw_thread_loop_destroy(pw_thread_loop_);
    pw_thread_loop_ = nullptr;
  }
  if (pw_stream_) {
    pw_stream_destroy(pw_stream_);
    pw_stream_ = nullptr;
  }
  if (pw_core_) { pw_core_disconnect(pw_core_); pw_core_ = nullptr; }
  if (pw_ctx_) { pw_context_destroy(pw_ctx_); pw_ctx_ = nullptr; }
  stream_connected_.store(false);
}

bool PipewireCapture::grab(CapturedFrame& frame, int timeout_ms) {
  std::unique_lock<std::mutex> lk(frame_mutex_);
  if (latest_pixels_.empty()) {
    if (!frame_cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                            [this] { return !latest_pixels_.empty() || !running_.load(); })) {
      return false;
    }
  }
  if (latest_pixels_.empty()) return false;
  frame.width = latest_width_;
  frame.height = latest_height_;
  frame.stride = latest_stride_;
  frame.format = PixelFormat::kBGRA;
  frame.capture_ts_us = latest_ts_us_;
  frame.pixels = latest_pixels_;
  return true;
}

}  // namespace rc
