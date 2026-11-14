#pragma once

#include <string>
#include <unordered_set>

namespace rc {

// 通过 Windows SendInput 向当前交互桌面注入鼠标、滚轮和键盘事件。
class WindowsInputInjector {
 public:
  WindowsInputInjector() = default;
  ~WindowsInputInjector();

  bool start(int width, int height, std::string* error);
  void stop();

  void mouse_move(int x, int y);
  void mouse_button(int button, bool pressed, int x, int y);
  void mouse_wheel(int delta_x, int delta_y);
  void key(int qt_key, int state);

 private:
  void send_mouse(unsigned long flags, unsigned long data = 0, int x = 0, int y = 0);
  void send_key(unsigned short key, bool pressed);

  int width_ = 0;
  int height_ = 0;
  std::unordered_set<unsigned short> pressed_keys_;
  std::unordered_set<int> pressed_buttons_;
};

}  // namespace rc
