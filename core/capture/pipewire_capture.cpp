#include "capture/pipewire_capture.h"

#include <pipewire/pipewire.h>
#include <pipewire/stream.h>
#include <pipewire/core.h>
#include <pipewire/context.h>
#include <pipewire/thread-loop.h>
#include <pipewire/properties.h>
#include <spa/param/video/raw.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/buffers.h>
#include <spa/param/param-types.h>
#include <spa/pod/builder.h>
#include <spa/pod/parser.h>
#include <spa/utils/result.h>
#include <spa/utils/defs.h>

#include <libportal/portal.h>
#include <libportal/remote.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>

#include "base/log.h"

namespace rc {

namespace {

// pw_init/pw_deinit 是进程全局的，用引用计数管理。
std::atomic<int>& pw_refs() {
  static std::atomic<int> r{0};
  return r;
}
void pw_global_init() {
  if (pw_refs().fetch_add(1) == 0) pw_init(nullptr, nullptr);
}
void pw_global_deinit() {
  if (pw_refs().fetch_sub(1) == 1) pw_deinit();
}

// ── restore token 持久化：二次运行免弹窗 ────────────────────────────────
std::string token_path() {
  const char* xdg = g_getenv("XDG_CACHE_HOME");
  std::string base = (xdg && *xdg)
                         ? xdg
                         : std::string(g_get_home_dir() ? g_get_home_dir() : "/tmp") + "/.cache";
  return base + "/ai_control_pc/screencast_restore_token";
}

std::string load_token() {
  std::string path = token_path();
  gchar* contents = nullptr;
  if (!g_file_get_contents(path.c_str(), &contents, nullptr, nullptr) || !contents) {
    return {};
  }
  std::string token(contents);
  g_free(contents);
  while (!token.empty() &&
         (token.back() == '\n' || token.back() == '\r' || token.back() == ' ')) {
    token.pop_back();
  }
  return token;
}

void save_token(const char* token) {
  if (!token || !*token) return;
  std::string path = token_path();
  gchar* dir = g_path_get_dirname(path.c_str());
  g_mkdir_with_parents(dir, 0700);
  g_free(dir);
  g_file_set_contents(path.c_str(), token, -1, nullptr);
}

// ── 把 libportal 的异步操作同步化 ───────────────────────────────────────
struct WaitData {
  GMainLoop* loop = nullptr;
  GAsyncResult* result = nullptr;
};

static void on_async_done(GObject*, GAsyncResult* res, gpointer user_data) {
  auto* wd = static_cast<WaitData*>(user_data);
  wd->result = G_ASYNC_RESULT(g_object_ref(res));
  if (wd->loop) g_main_loop_quit(wd->loop);
}

template <typename Launch>
static GAsyncResult* run_and_wait(GMainContext* ctx, Launch launch) {
  WaitData wd;
  wd.loop = g_main_loop_new(ctx, FALSE);
  launch(on_async_done, &wd);
  g_main_loop_run(wd.loop);
  g_main_loop_unref(wd.loop);
  return wd.result;
}

}  // namespace

PipewireCapture::PipewireCapture() = default;

PipewireCapture::~PipewireCapture() { stop(); }

bool PipewireCapture::start(int monitor_index, std::string* error) {
  if (running_.load()) return true;

  pw_global_init();

  if (!run_portal(monitor_index, error)) {
    pw_global_deinit();
    return false;
  }

  if (!connect_pipewire(error)) {
    destroy_pipewire();
    pw_global_deinit();
    return false;
  }

  if (!init_stream(target_node_id_, error)) {
    destroy_pipewire();
    pw_global_deinit();
    return false;
  }

  running_.store(true);
  RC_LOGI("pw", "screencast started: " << width_ << "x" << height_ << " " << format_);
  return true;
}

// ── Portal：拿 PipeWire fd + node_id ────────────────────────────────────
bool PipewireCapture::run_portal(int /*monitor_index*/, std::string* error) {
  portal_ = xdp_portal_new();
  if (!portal_) {
    if (error) *error = "xdp_portal_new failed";
    return false;
  }

  GMainContext* ctx = g_main_context_new();
  g_main_context_push_thread_default(ctx);

  bool ok = false;
  GError* ger = nullptr;

  // 1. 创建 screencast session（带 restore token 可免弹窗）
  std::string token = load_token();
  GAsyncResult* r = run_and_wait(ctx, [&](GAsyncReadyCallback cb, gpointer data) {
    xdp_portal_create_screencast_session(
        portal_,
        XDP_OUTPUT_MONITOR,
        XDP_SCREENCAST_FLAG_NONE,
        XDP_CURSOR_MODE_EMBEDDED,
        XDP_PERSIST_MODE_PERSISTENT,
        token.empty() ? nullptr : token.c_str(),
        nullptr, cb, data);
  });
  session_ = xdp_portal_create_screencast_session_finish(portal_, r, &ger);
  if (r) g_object_unref(r);

  // token 失效则重试一次（走弹窗）
  if (!session_ && !token.empty()) {
    if (ger) { g_error_free(ger); ger = nullptr; }
    g_unlink(token_path().c_str());
    r = run_and_wait(ctx, [&](GAsyncReadyCallback cb, gpointer data) {
      xdp_portal_create_screencast_session(
          portal_, XDP_OUTPUT_MONITOR, XDP_SCREENCAST_FLAG_NONE,
          XDP_CURSOR_MODE_EMBEDDED, XDP_PERSIST_MODE_PERSISTENT,
          nullptr, nullptr, cb, data);
    });
    session_ = xdp_portal_create_screencast_session_finish(portal_, r, &ger);
    if (r) g_object_unref(r);
  }

  if (!session_) {
    if (error) *error = std::string("create screencast session: ") +
                         (ger ? ger->message : "unknown");
    if (ger) g_error_free(ger);
    goto done;
  }

  // 2. 启动 session（首次会弹窗让用户选屏）
  r = run_and_wait(ctx, [&](GAsyncReadyCallback cb, gpointer data) {
    xdp_session_start(session_, nullptr, nullptr, cb, data);
  });
  if (!xdp_session_start_finish(session_, r, &ger)) {
    if (error) *error = std::string("start screencast session: ") +
                         (ger ? ger->message : "cancelled by user");
    if (ger) g_error_free(ger);
    if (r) g_object_unref(r);
    goto done;
  }
  if (r) g_object_unref(r);

  // 持久化 restore token
  if (char* t = xdp_session_get_restore_token(session_)) {
    save_token(t);
    g_free(t);
  }

  // 3. 打开 PipeWire 远程 fd
  pw_remote_fd_ = xdp_session_open_pipewire_remote(session_);
  if (pw_remote_fd_ < 0) {
    if (error) *error = "xdp_session_open_pipewire_remote failed";
    goto done;
  }

  // 4. 读取 stream 信息（精确 node_id）
  {
    GVariant* streams = xdp_session_get_streams(session_);
    if (!streams) {
      if (error) *error = "xdp_session_get_streams returned null";
      goto done;
    }
    gsize n = g_variant_n_children(streams);
    if (n == 0) {
      g_variant_unref(streams);
      if (error) *error = "portal returned no streams";
      goto done;
    }
    // 取第一个 stream（多屏场景可扩展）
    GVariant* item = g_variant_get_child_value(streams, 0);
    GVariantIter* it = nullptr;
    guint32 node_id = 0;
    g_variant_get(item, "(ua{sv})", &node_id, &it);
    target_node_id_ = node_id;
    g_variant_iter_free(it);
    g_variant_unref(item);
    g_variant_unref(streams);
  }

  RC_LOGI("pw", "portal: node_id=" << target_node_id_);
  ok = true;

done:
  g_main_context_pop_thread_default(ctx);
  g_main_context_unref(ctx);
  return ok;
}

// ── PipeWire 连接（直连 portal 给的 fd）─────────────────────────────────
bool PipewireCapture::connect_pipewire(std::string* error) {
  loop_ = pw_thread_loop_new("ai-ctrl-pw", nullptr);
  if (!loop_) { if (error) *error = "pw_thread_loop_new failed"; return false; }

  ctx_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
  if (!ctx_) { if (error) *error = "pw_context_new failed"; return false; }

  core_ = pw_context_connect_fd(ctx_, pw_remote_fd_, nullptr, 0);
  if (!core_) { if (error) *error = "pw_context_connect_fd failed"; return false; }

  if (pw_thread_loop_start(loop_) != 0) {
    if (error) *error = "pw_thread_loop_start failed";
    return false;
  }
  return true;
}

// ── 创建 pw_stream 并协商格式 ───────────────────────────────────────────
bool PipewireCapture::init_stream(uint32_t node_id, std::string* error) {
  auto* props = pw_properties_new(
      PW_KEY_APP_NAME, "ai-control-pc",
      PW_KEY_APP_ID, "ai-control-pc",
      PW_KEY_MEDIA_ROLE, "screencast-capture",
      nullptr);
  stream_ = pw_stream_new(core_, "ai-ctrl-capture", props);
  if (!stream_) { if (error) *error = "pw_stream_new failed"; return false; }

  static const pw_stream_events events = {
      PW_VERSION_STREAM_EVENTS,
      nullptr,  // destroy
      &PipewireCapture::stream_state_changed,
      nullptr,  // control_info
      nullptr,  // io_changed
      &PipewireCapture::stream_param_changed,
      nullptr,  // add_buffer
      nullptr,  // remove_buffer
      &PipewireCapture::stream_process,
      nullptr,  // drained
      nullptr,  // command
      nullptr,  // trigger_done
  };
  pw_stream_add_listener(stream_, &stream_listener_, &events, this);

  // 格式提案：用 CHOICE 范围，兼容任意分辨率/帧率。
  // 只请求 4 字节 RGB 族（编码器按 BGRA 处理），BGRx 的 X 通道被忽略。
  uint8_t buf[4096];
  spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));

  spa_rectangle def_size = SPA_RECTANGLE(1920, 1080);
  spa_rectangle min_size = SPA_RECTANGLE(1, 1);
  spa_rectangle max_size = SPA_RECTANGLE(7680, 4320);
  spa_fraction def_rate = SPA_FRACTION(30, 1);
  spa_fraction min_rate = SPA_FRACTION(0, 1);
  spa_fraction max_rate = SPA_FRACTION(120, 1);

  const spa_pod* params[2];
  int n = 0;
  params[n++] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
      &b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
      SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
      SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
      SPA_FORMAT_VIDEO_format,
          SPA_POD_CHOICE_ENUM_Id(4, SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_BGRx,
                                 SPA_VIDEO_FORMAT_RGBA, SPA_VIDEO_FORMAT_RGBx),
      SPA_FORMAT_VIDEO_size,
          SPA_POD_CHOICE_RANGE_Rectangle(&def_size, &min_size, &max_size),
      SPA_FORMAT_VIDEO_framerate,
          SPA_POD_CHOICE_RANGE_Fraction(&def_rate, &min_rate, &max_rate),
      0));

  params[n++] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
      &b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
      SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(8, 2, 16),
      SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(1 << SPA_DATA_MemPtr),
      0));

  pw_thread_loop_lock(loop_);
  int ret = pw_stream_connect(stream_, PW_DIRECTION_INPUT,
                              static_cast<int32_t>(node_id),
                              static_cast<pw_stream_flags>(
                                  PW_STREAM_FLAG_AUTOCONNECT |
                                  PW_STREAM_FLAG_MAP_BUFFERS),
                              params, n);
  pw_thread_loop_unlock(loop_);
  if (ret < 0) {
    if (error) *error = std::string("pw_stream_connect: ") + spa_strerror(ret);
    return false;
  }

  // 等待格式协商完成（有界）
  {
    std::unique_lock<std::mutex> lk(fmt_mu_);
    if (!fmt_cv_.wait_for(lk, std::chrono::seconds(5),
                          [this] { return negotiated_; })) {
      if (error) *error = "format negotiation timed out";
      return false;
    }
  }
  return true;
}

void PipewireCapture::destroy_pipewire() {
  stopping_.store(true);

  if (loop_) pw_thread_loop_stop(loop_);

  if (stream_) {
    if (loop_) {
      pw_thread_loop_lock(loop_);
      pw_stream_destroy(stream_);
      pw_thread_loop_unlock(loop_);
    } else {
      pw_stream_destroy(stream_);
    }
    stream_ = nullptr;
  }
  stream_gone_.store(true);

  if (core_) { pw_core_disconnect(core_); core_ = nullptr; }
  if (ctx_) { pw_context_destroy(ctx_); ctx_ = nullptr; }
  if (loop_) { pw_thread_loop_destroy(loop_); loop_ = nullptr; }
  if (pw_remote_fd_ >= 0) { ::close(pw_remote_fd_); pw_remote_fd_ = -1; }

  if (session_) {
    xdp_session_close(session_);
    g_object_unref(session_);
    session_ = nullptr;
  }
  if (portal_) {
    g_object_unref(portal_);
    portal_ = nullptr;
  }
}

void PipewireCapture::stop() {
  if (!running_.load() && !loop_ && !portal_) return;
  running_.store(false);
  destroy_pipewire();
  pw_global_deinit();
}

// ── grab()：从最新帧缓冲取帧 ────────────────────────────────────────────
bool PipewireCapture::grab(CapturedFrame& frame, int timeout_ms) {
  std::unique_lock<std::mutex> lk(frame_mu_);
  if (!frame_ready_) {
    if (!frame_cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                            [this] { return frame_ready_ || !running_.load(); })) {
      return false;
    }
  }
  if (!frame_ready_) return false;

  frame.width = static_cast<int>(width_);
  frame.height = static_cast<int>(height_);
  frame.stride = static_cast<int>(stride_);
  frame.format = PixelFormat::kBGRA;
  frame.capture_ts_us = static_cast<int64_t>(latest_ts_us_);
  frame.pixels.swap(latest_pixels_);
  frame_ready_ = false;
  return true;
}

// ── PipeWire 回调 ───────────────────────────────────────────────────────
void PipewireCapture::stream_state_changed(void*, enum pw_stream_state old,
                                           enum pw_stream_state state, const char* err) {
  RC_LOGI("pw", "stream state: " << pw_stream_state_as_string(old) << " -> "
                                  << pw_stream_state_as_string(state)
                                  << (err ? err : ""));
}

void PipewireCapture::stream_param_changed(void* data, uint32_t id,
                                           const spa_pod* param) {
  auto* self = static_cast<PipewireCapture*>(data);
  if (id != SPA_PARAM_Format || !param) return;

  uint32_t fmt_id = 0;
  spa_rectangle size = {0, 0};
  uint32_t type_id = SPA_PARAM_Format;
  spa_pod_parse_object(param, SPA_TYPE_OBJECT_Format, &type_id,
      SPA_POD_Prop(SPA_FORMAT_VIDEO_format), SPA_POD_Id(&fmt_id),
      SPA_POD_Prop(SPA_FORMAT_VIDEO_size), SPA_POD_Rectangle(&size),
      0);

  self->width_ = size.width;
  self->height_ = size.height;

  if (fmt_id == SPA_VIDEO_FORMAT_BGRA) self->format_ = "BGRA";
  else if (fmt_id == SPA_VIDEO_FORMAT_BGRx) self->format_ = "BGRx";
  else if (fmt_id == SPA_VIDEO_FORMAT_RGBA) self->format_ = "RGBA";
  else if (fmt_id == SPA_VIDEO_FORMAT_RGBx) self->format_ = "RGBx";
  else self->format_ = "UNKNOWN";

  self->stride_ = self->width_ * 4;

  RC_LOGI("pw", "negotiated: " << self->width_ << "x" << self->height_ << " "
                                << self->format_);
  {
    std::lock_guard<std::mutex> lk(self->fmt_mu_);
    self->negotiated_ = true;
  }
  self->fmt_cv_.notify_all();
}

void PipewireCapture::stream_process(void* data) {
  auto* self = static_cast<PipewireCapture*>(data);
  if (self->stopping_.load()) return;

  while (true) {
    pw_buffer* buf = pw_stream_dequeue_buffer(self->stream_);
    if (!buf) break;

    spa_buffer* sb = buf->buffer;
    if (!sb || sb->n_datas < 1) {
      pw_stream_queue_buffer(self->stream_, buf);
      continue;
    }
    spa_data* d = &sb->datas[0];
    if (!d->data || !d->chunk || d->chunk->size == 0) {
      pw_stream_queue_buffer(self->stream_, buf);
      continue;
    }

    const size_t payload = d->chunk->size;
    const int32_t src_stride = d->chunk->stride;
    const uint32_t stride =
        src_stride > 0 ? static_cast<uint32_t>(src_stride) : self->width_ * 4;

    const uint8_t* src = static_cast<const uint8_t*>(d->data) + d->chunk->offset;

    // 拷贝到最新帧缓冲。RGBA/RGBx 需要 R/B 交换（编码器按 BGRA 处理）。
    bool swap_rb = (self->format_ == "RGBA" || self->format_ == "RGBx");

    {
      std::lock_guard<std::mutex> lk(self->frame_mu_);
      self->latest_pixels_.resize(payload);
      if (swap_rb) {
        uint8_t* dst = self->latest_pixels_.data();
        for (size_t i = 0; i < payload; i += 4) {
          dst[i]     = src[i + 2];  // B
          dst[i + 1] = src[i + 1];  // G
          dst[i + 2] = src[i];      // R
          dst[i + 3] = src[i + 3];  // A/X
        }
      } else {
        std::memcpy(self->latest_pixels_.data(), src, payload);
      }
      self->latest_ts_us_ = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::microseconds>(
              std::chrono::steady_clock::now().time_since_epoch())
              .count());
      self->stride_ = stride;
      self->frame_ready_ = true;
    }
    self->frame_cv_.notify_all();

    pw_stream_queue_buffer(self->stream_, buf);
  }
}

}  // namespace rc
