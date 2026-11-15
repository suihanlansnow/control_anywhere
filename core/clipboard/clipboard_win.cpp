#include "clipboard/clipboard.h"

#include <windows.h>

#include <cstring>
#include <string>

namespace rc {
namespace {

std::wstring utf8_to_wide(const std::string& text) {
  if (text.empty()) return {};
  const int len = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                      nullptr, 0);
  if (len <= 0) return {};
  std::wstring wide(static_cast<std::size_t>(len), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), len);
  return wide;
}

std::string wide_to_utf8(const wchar_t* text) {
  const int len = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  if (len <= 1) return {};
  std::string out(static_cast<std::size_t>(len - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), len, nullptr, nullptr);
  return out;
}

// 剪贴板同一时刻只能被一个进程占用，偶发失败时短暂重试。
bool open_clipboard() {
  for (int i = 0; i < 5; ++i) {
    if (OpenClipboard(nullptr)) return true;
    Sleep(10);
  }
  return false;
}

class WindowsClipboard : public Clipboard {
 public:
  bool read_text(std::string& text, std::string* error) override {
    text.clear();
    if (!open_clipboard()) {
      if (error) *error = "OpenClipboard failed";
      return false;
    }
    HANDLE handle = GetClipboardData(CF_UNICODETEXT);
    if (!handle) {
      CloseClipboard();
      return true;  // 剪贴板为空或不是文本
    }
    const wchar_t* data = static_cast<const wchar_t*>(GlobalLock(handle));
    if (!data) {
      CloseClipboard();
      if (error) *error = "GlobalLock failed";
      return false;
    }
    text = wide_to_utf8(data);
    GlobalUnlock(handle);
    CloseClipboard();
    return true;
  }

  bool write_text(const std::string& text, std::string* error) override {
    const std::wstring wide = utf8_to_wide(text);
    if (!open_clipboard()) {
      if (error) *error = "OpenClipboard failed";
      return false;
    }
    if (!EmptyClipboard()) {
      CloseClipboard();
      if (error) *error = "EmptyClipboard failed";
      return false;
    }
    const std::size_t bytes = (wide.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) {
      CloseClipboard();
      if (error) *error = "GlobalAlloc failed";
      return false;
    }
    void* dest = GlobalLock(memory);
    if (!dest) {
      GlobalFree(memory);
      CloseClipboard();
      if (error) *error = "GlobalLock failed";
      return false;
    }
    std::memcpy(dest, wide.c_str(), bytes);
    GlobalUnlock(memory);
    if (!SetClipboardData(CF_UNICODETEXT, memory)) {
      // 成功后所有权归系统，只有失败时才需要自己释放。
      GlobalFree(memory);
      CloseClipboard();
      if (error) *error = "SetClipboardData failed";
      return false;
    }
    CloseClipboard();
    return true;
  }
};

}  // namespace

std::unique_ptr<Clipboard> create_clipboard(std::string* error) {
  (void)error;
  return std::make_unique<WindowsClipboard>();
}

}  // namespace rc
