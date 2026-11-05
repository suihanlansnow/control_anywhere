#include "capture/x11_capture.h"

#include <X11/extensions/XShm.h>
#include <X11/Xutil.h>

#include <sys/ipc.h>
#include <sys/shm.h>

#include <chrono>
#include <cstring>
#include <thread>

#include "base/clock.h"
#include "base/log.h"

namespace rc {

namespace {

// 统一的 XShm 资源释放：detach（仅已 attach 时）→ 解绑 data 指针 → 销毁 XImage
// → shmdt → shmctl(IPC_RMID) → delete。
// start() 各阶段失败（shmid 已有、shmaddr 尚未建立、尚未 attach）也走这一条，避免泄漏。
static void release_xshm(Display* display, XShmSegmentInfo*& info, XImage*& img,
                         unsigned char*& shm_data, bool attached) {
  if (attached && display && info) XShmDetach(display, info);
  if (img) {
    img->data = nullptr;  // 阻止 XDestroyImage 去 free 共享内存
    XDestroyImage(img);
    img = nullptr;
  }
  shm_data = nullptr;
  if (info) {
    if (info->shmaddr) shmdt(info->shmaddr);
    if (info->shmid > 0) shmctl(info->shmid, IPC_RMID, nullptr);
    delete info;
    info = nullptr;
  }
}

}  // namespace

X11Capture::X11Capture() = default;

X11Capture::~X11Capture() { stop(); }

bool X11Capture::start(int monitor_index, std::string* error) {
  if (started_) return true;

  display_ = XOpenDisplay(nullptr);
  if (!display_) {
    if (error) *error = "XOpenDisplay failed, no X server";
    return false;
  }

  screen_ = monitor_index;
  if (screen_ >= XScreenCount(display_)) {
    screen_ = 0;
  }

  Screen* scr = XScreenOfDisplay(display_, screen_);
  root_ = scr->root;
  width_ = scr->width;
  height_ = scr->height;

  RC_LOGI("x11", "screen " << screen_ << " " << width_ << "x" << height_);

  int depth = XDefaultDepth(display_, screen_);
  Visual* visual = XDefaultVisual(display_, screen_);

  if (XShmQueryExtension(display_)) {
    XShmSegmentInfo* info = new XShmSegmentInfo{};
    XImage* img = XShmCreateImage(display_, visual, depth, ZPixmap,
                                  nullptr, info, width_, height_);
    bool attached = false;
    bool ready = false;

    if (!img) {
      RC_LOGW("x11", "XShmCreateImage failed, falling back to XGetImage");
    } else {
      const size_t img_size = static_cast<size_t>(img->bytes_per_line) * img->height;
      const int shmid = shmget(IPC_PRIVATE, img_size, IPC_CREAT | 0600);
      if (shmid == -1) {
        RC_LOGW("x11", "shmget failed, falling back to XGetImage");
      } else {
        // 先记录 shmid，后续任何一步失败都能在统一回收里 IPC_RMID 掉这个段。
        info->shmid = shmid;
        char* addr = static_cast<char*>(shmat(shmid, nullptr, 0));
        if (addr == reinterpret_cast<char*>(-1)) {
          RC_LOGW("x11", "shmat failed, falling back to XGetImage");
        } else {
          info->shmaddr = addr;
          img->data = addr;
          if (!XShmAttach(display_, info)) {
            RC_LOGW("x11", "XShmAttach failed, falling back to XGetImage");
          } else {
            attached = true;
            info->readOnly = true;
            shm_data_ = reinterpret_cast<unsigned char*>(addr);
            shm_info_ = info;
            shm_img_ = img;
            use_xshm_ = true;
            ready = true;
            RC_LOGI("x11", "using XShm shared memory capture");
          }
        }
      }
    }

    if (!ready) {
      release_xshm(display_, info, img, shm_data_, attached);
      shm_info_ = nullptr;
      shm_img_ = nullptr;
      use_xshm_ = false;
    }
  }

  if (!use_xshm_) {
    XImage* probe = XGetImage(display_, root_, 0, 0, 1, 1, AllPlanes, ZPixmap);
    if (probe) {
      depth_ = probe->depth;
      bits_per_pixel_ = probe->bits_per_pixel;
      red_mask_ = probe->red_mask;
      green_mask_ = probe->green_mask;
      blue_mask_ = probe->blue_mask;
      XDestroyImage(probe);
    }
  }

  started_ = true;
  return true;
}

void X11Capture::stop() {
  if (!started_) return;
  started_ = false;

  if (shm_info_ != nullptr) {
    auto* info = static_cast<XShmSegmentInfo*>(shm_info_);
    release_xshm(display_, info, shm_img_, shm_data_, true);
    shm_info_ = nullptr;
    use_xshm_ = false;
  }

  if (display_) {
    XCloseDisplay(display_);
    display_ = nullptr;
  }
  last_grab_us_ = 0;
}

static bool ximage_to_bgra_fast(XImage* img, CapturedFrame& frame) {
  int w = img->width;
  int h = img->height;
  int bpl = img->bytes_per_line;

  frame.width = w;
  frame.height = h;
  frame.stride = w * 4;
  frame.format = PixelFormat::kBGRA;
  frame.pixels.resize(static_cast<size_t>(w) * h * 4);

  uint32_t rmask = img->red_mask;
  uint32_t gmask = img->green_mask;
  uint32_t bmask = img->blue_mask;

  if (img->bits_per_pixel == 32 &&
      rmask == 0x00FF0000 && gmask == 0x0000FF00 && bmask == 0x000000FF) {
    const uint8_t* data = reinterpret_cast<const uint8_t*>(img->data);
    for (int y = 0; y < h; ++y) {
      std::memcpy(frame.pixels.data() + y * frame.stride,
                  data + y * bpl, w * 4);
    }
    return true;
  }

  // 非标准掩码（RGB565、RGB10 等）：把 bpp 分支提到循环外（原来逐像素判断），
  // 并按通道位宽缩放，避免低位通道（如 RGB565 的 5/6 位）被直接截断丢失精度。
  int rshift = 0, gshift = 0, bshift = 0;
  for (int i = 0; i < 32; ++i) {
    if (rmask & (1u << i)) rshift = i;
    if (gmask & (1u << i)) gshift = i;
    if (bmask & (1u << i)) bshift = i;
  }
  const int rbits = __builtin_popcount(rmask);
  const int gbits = __builtin_popcount(gmask);
  const int bbits = __builtin_popcount(bmask);

  auto to_u8 = [](uint32_t value, int bits) -> uint8_t {
    if (bits <= 0) return 0;
    if (bits >= 8) return static_cast<uint8_t>(value >> (bits - 8));
    return static_cast<uint8_t>(value << (8 - bits));
  };

  const uint8_t* data = reinterpret_cast<const uint8_t*>(img->data);
  if (img->bits_per_pixel == 32) {
    for (int y = 0; y < h; ++y) {
      const uint8_t* src_row = data + y * bpl;
      uint8_t* dst_row = frame.pixels.data() + y * frame.stride;
      for (int x = 0; x < w; ++x) {
        const uint32_t pixel = *reinterpret_cast<const uint32_t*>(src_row + x * 4);
        dst_row[x * 4 + 0] = to_u8((pixel & bmask) >> bshift, bbits);
        dst_row[x * 4 + 1] = to_u8((pixel & gmask) >> gshift, gbits);
        dst_row[x * 4 + 2] = to_u8((pixel & rmask) >> rshift, rbits);
        dst_row[x * 4 + 3] = 0xFF;
      }
    }
    return true;
  }

  if (img->bits_per_pixel == 16) {
    for (int y = 0; y < h; ++y) {
      const uint8_t* src_row = data + y * bpl;
      uint8_t* dst_row = frame.pixels.data() + y * frame.stride;
      for (int x = 0; x < w; ++x) {
        const uint32_t pixel = *reinterpret_cast<const uint16_t*>(src_row + x * 2);
        dst_row[x * 4 + 0] = to_u8((pixel & bmask) >> bshift, bbits);
        dst_row[x * 4 + 1] = to_u8((pixel & gmask) >> gshift, gbits);
        dst_row[x * 4 + 2] = to_u8((pixel & rmask) >> rshift, rbits);
        dst_row[x * 4 + 3] = 0xFF;
      }
    }
    return true;
  }

  // 其他位深（如 8bpp 调色板）无法从掩码还原颜色，输出不透明黑，保持原有兜底行为。
  for (int y = 0; y < h; ++y) {
    uint8_t* dst_row = frame.pixels.data() + y * frame.stride;
    for (int x = 0; x < w; ++x) {
      dst_row[x * 4 + 0] = 0;
      dst_row[x * 4 + 1] = 0;
      dst_row[x * 4 + 2] = 0;
      dst_row[x * 4 + 3] = 0xFF;
    }
  }
  return true;
}

bool X11Capture::grab(CapturedFrame& frame, int timeout_ms) {
  if (!started_ || !display_) return false;

  // 按调用方给的间隔节流，模拟“等新帧”的语义，避免空转。
  if (timeout_ms > 0 && last_grab_us_ != 0) {
    const uint64_t period_us = static_cast<uint64_t>(timeout_ms) * 1000ULL;
    const uint64_t now = now_us();
    const uint64_t elapsed = now - last_grab_us_;
    if (elapsed < period_us) {
      std::this_thread::sleep_for(std::chrono::microseconds(period_us - elapsed));
    }
  }

  XImage* img = nullptr;

  if (use_xshm_ && shm_img_) {
    if (!XShmGetImage(display_, root_, shm_img_, 0, 0, AllPlanes)) {
      // 运行时共享内存失效（X server 重启、segment 被外部清理等）——
      // 必须在回退到 XGetImage 前把自己持有的 segment 彻底释放，否则进程结束前一直泄漏。
      RC_LOGW("x11", "XShmGetImage failed, falling back to XGetImage");
      auto* info = static_cast<XShmSegmentInfo*>(shm_info_);
      release_xshm(display_, info, shm_img_, shm_data_, true);
      shm_info_ = nullptr;
      use_xshm_ = false;
    } else {
      img = shm_img_;
    }
  }

  if (!img) {
    img = XGetImage(display_, root_, 0, 0, width_, height_, AllPlanes, ZPixmap);
    if (!img) {
      RC_LOGW("x11", "XGetImage failed");
      return false;
    }
  }

  frame.capture_ts_us = static_cast<int64_t>(now_us());
  ximage_to_bgra_fast(img, frame);

  if (!use_xshm_) {
    XDestroyImage(img);
  }
  last_grab_us_ = now_us();
  return true;
}

bool X11Capture::set_output_format(PixelFormat format) {
  desired_format_ = format;
  return format == PixelFormat::kBGRA;
}

}  // namespace rc
