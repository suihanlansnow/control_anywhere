#include "capture/capture.h"

#include "capture/synthetic_capture.h"

#ifdef _WIN32
#include "capture/dxgi_capture.h"
#endif

#ifdef __linux__
#include "capture/x11_capture.h"
#endif

namespace rc {

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

#ifdef __linux__
  if (error && (kind.empty() || kind == "auto" || kind == "default")) {
    return std::make_unique<X11Capture>();
  }
#endif

  if (error) *error = "unknown capture source: " + kind;
  return nullptr;
}

}  // namespace rc
