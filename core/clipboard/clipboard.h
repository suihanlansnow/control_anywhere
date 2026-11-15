#pragma once

#include <memory>
#include <string>

namespace rc {

// 系统剪贴板文本读写。Windows 走原生 API；Linux 依赖 wl-clipboard 或 xclip/xsel。
class Clipboard {
 public:
  virtual ~Clipboard() = default;
  // 读取纯文本；剪贴板为空时返回 true 且 text 为空。
  virtual bool read_text(std::string& text, std::string* error) = 0;
  virtual bool write_text(const std::string& text, std::string* error) = 0;
};

// 创建平台实现；当前平台不支持时返回 nullptr。
std::unique_ptr<Clipboard> create_clipboard(std::string* error);

}  // namespace rc
