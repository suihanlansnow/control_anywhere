#include "capture/capture.h"

#include "capture/synthetic_capture.h"

#ifdef _WIN32
#include "capture/dxgi_capture.h"
#endif

#ifdef __linux__
#include "capture/x11_capture.h"
#endif

#if defined(__linux__) && defined(HAVE_PIPEWIRE_CAPTURE)
#include "capture/pipewire_capture.h"
#endif

namespace rc {

static bool is_wayland_session() {
  const char* t = std::getenv("XDG_SESSION_TYPE");
  return t && std::string(t) == "wayland";
}

std::unique_ptr<ICaptureSource> create_capture_source(const std::string& kind,
                                                      std::string* error) {
  if (kind == "synthetic") {
    return std::make_unique<SyntheticCapture>();
  }

  if (kind == "dxgi") {
#ifdef _WIN32
    return std::make_unique<DxgiCapture>();
#else
    if (error) *error = "dxgi capture is only available on Windows";
    return nullptr;
#endif
  }

  if (kind == "x11") {
#ifdef __linux__
    return std::make_unique<X11Capture>();
#else
    if (error) *error = "x11 capture is only available on Linux";
    return nullptr;
#endif
  }

  if (kind == "pipewire") {
#if defined(__linux__) && defined(HAVE_PIPEWIRE_CAPTURE)
    return std::make_unique<PipewireCapture>();
#else
    if (error) *error = "pipewire capture not built / only on Linux";
    return nullptr;
#endif
  }

#ifdef __linux__
  if (kind.empty() || kind == "auto" || kind == "default") {
#if defined(HAVE_PIPEWIRE_CAPTURE)
    if (is_wayland_session()) {
      return std::make_unique<PipewireCapture>();
    }
#endif
    return std::make_unique<X11Capture>();
  }
#endif

  if (error) *error = "unknown capture source: " + kind;
  return nullptr;
}

}  // namespace rc
