#include "clipboard/clipboard.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace rc {
namespace {

bool wayland_session() {
  const char* display = std::getenv("WAYLAND_DISPLAY");
  return display != nullptr && display[0] != '\0';
}

// 读命令输出；返回 false 表示命令不可用或执行失败。
bool run_read(const char* command, std::string& out) {
  out.clear();
  FILE* pipe = popen(command, "r");
  if (!pipe) return false;
  char buffer[4096];
  while (true) {
    const std::size_t got = std::fread(buffer, 1, sizeof(buffer), pipe);
    if (got == 0) break;
    out.append(buffer, got);
  }
  return pclose(pipe) == 0;
}

bool run_write(const char* command, const std::string& text) {
  FILE* pipe = popen(command, "w");
  if (!pipe) return false;
  const std::size_t written = std::fwrite(text.data(), 1, text.size(), pipe);
  return pclose(pipe) == 0 && written == text.size();
}

// Linux 没有统一剪贴板 API：Wayland 用 wl-clipboard，X11 用 xclip/xsel。
class LinuxClipboard : public Clipboard {
 public:
  bool read_text(std::string& text, std::string* error) override {
    (void)error;
    if (wayland_session()) {
      if (run_read("wl-paste --no-newline 2>/dev/null", text)) return true;
    } else {
      if (run_read("xclip -selection clipboard -o 2>/dev/null", text)) return true;
      if (run_read("xsel --clipboard --output 2>/dev/null", text)) return true;
    }
    // 剪贴板为空或工具不可用时都按“无内容”处理，避免轮询时反复报错。
    text.clear();
    return true;
  }

  bool write_text(const std::string& text, std::string* error) override {
    if (wayland_session()) {
      if (run_write("wl-copy 2>/dev/null", text)) return true;
    } else {
      if (run_write("xclip -selection clipboard 2>/dev/null", text)) return true;
      if (run_write("xsel --clipboard --input 2>/dev/null", text)) return true;
    }
    if (error) *error = "no clipboard tool (install wl-clipboard or xclip)";
    return false;
  }
};

}  // namespace

std::unique_ptr<Clipboard> create_clipboard(std::string* error) {
  (void)error;
  return std::make_unique<LinuxClipboard>();
}

}  // namespace rc
