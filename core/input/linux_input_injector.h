#pragma once

#include <cstdint>
#include <string>

namespace rc {

// 通过内核 uinput 创建虚拟键鼠，Wayland 与 X11 均走系统原生输入管线。
class LinuxInputInjector {
 public:
  LinuxInputInjector();
  ~LinuxInputInjector();

  bool start(int width, int height, std::string* error);
  void stop();

  void mouse_move(int x, int y);
  void mouse_button(int button, bool pressed, int x, int y);
  void mouse_wheel(int delta_x, int delta_y);
  void key(int qt_key, int state);

 private:
  bool emit(uint16_t type, uint16_t code, int32_t value);
  bool sync();

  int fd_ = -1;
  int width_ = 0;
  int height_ = 0;
  int wheel_remainder_x_ = 0;
  int wheel_remainder_y_ = 0;
};

}  // namespace rc
