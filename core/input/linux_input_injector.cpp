#include "input/linux_input_injector.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <iterator>
#include <linux/input.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace rc {
namespace {

int qt_key_to_linux(int key) {
  // Qt::Key 的字母和数字码与 ASCII 大写字符一致，协议中只传整数值，
  // 被控端不依赖 Qt 也能完成映射。Linux 字母键码并不连续，必须逐项映射。
  static constexpr int kLetterKeys[] = {
      KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I,
      KEY_J, KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R,
      KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z,
  };
  if (key >= 'A' && key <= 'Z') return kLetterKeys[key - 'A'];
  if (key >= '1' && key <= '9') return KEY_1 + (key - '1');
  if (key == '0') return KEY_0;

  switch (key) {
    case 0x01000000: return KEY_ESC;
    case 0x01000001: return KEY_TAB;
    case 0x01000003: return KEY_BACKSPACE;
    case 0x01000004:
    case 0x01000005: return KEY_ENTER;
    case 0x01000006: return KEY_INSERT;
    case 0x01000007: return KEY_DELETE;
    case 0x01000008: return KEY_PAUSE;
    case 0x01000009: return KEY_SYSRQ;
    case 0x01000010: return KEY_HOME;
    case 0x01000011: return KEY_END;
    case 0x01000012: return KEY_LEFT;
    case 0x01000013: return KEY_UP;
    case 0x01000014: return KEY_RIGHT;
    case 0x01000015: return KEY_DOWN;
    case 0x01000016: return KEY_PAGEUP;
    case 0x01000017: return KEY_PAGEDOWN;
    case 0x01000020: return KEY_LEFTSHIFT;
    case 0x01000021: return KEY_LEFTCTRL;
    case 0x01000022: return KEY_LEFTMETA;
    case 0x01000023: return KEY_LEFTALT;
    case 0x01000024: return KEY_CAPSLOCK;
    case 0x01000025: return KEY_NUMLOCK;
    case 0x01000026: return KEY_SCROLLLOCK;
    case 0x01000055: return KEY_COMPOSE;
    case 0x01001103: return KEY_RIGHTALT;
    case ' ': return KEY_SPACE;
    case '!': return KEY_1;
    case '@': return KEY_2;
    case '#': return KEY_3;
    case '$': return KEY_4;
    case '%': return KEY_5;
    case '^': return KEY_6;
    case '&': return KEY_7;
    case '*': return KEY_8;
    case '(': return KEY_9;
    case ')': return KEY_0;
    case ';':
    case ':': return KEY_SEMICOLON;
    case '=':
    case '+': return KEY_EQUAL;
    case ',':
    case '<': return KEY_COMMA;
    case '-':
    case '_': return KEY_MINUS;
    case '.':
    case '>': return KEY_DOT;
    case '/':
    case '?': return KEY_SLASH;
    case '`':
    case '~': return KEY_GRAVE;
    case '[':
    case '{': return KEY_LEFTBRACE;
    case '\\':
    case '|': return KEY_BACKSLASH;
    case ']':
    case '}': return KEY_RIGHTBRACE;
    case '\'':
    case '"': return KEY_APOSTROPHE;
    default: break;
  }

  static constexpr int kFunctionKeys[] = {
      KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6,
      KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
      KEY_F13, KEY_F14, KEY_F15, KEY_F16, KEY_F17, KEY_F18,
      KEY_F19, KEY_F20, KEY_F21, KEY_F22, KEY_F23, KEY_F24,
  };
  constexpr int kQtKeyF1 = 0x01000030;
  if (key >= kQtKeyF1 && key < kQtKeyF1 + static_cast<int>(std::size(kFunctionKeys))) {
    return kFunctionKeys[key - kQtKeyF1];
  }
  return 0;
}

int qt_button_to_linux(int button) {
  switch (button) {
    case 1: return BTN_LEFT;
    case 2: return BTN_RIGHT;
    case 3: return BTN_MIDDLE;
    case 4: return BTN_SIDE;
    case 5: return BTN_EXTRA;
    default: return 0;
  }
}

}  // namespace

LinuxInputInjector::LinuxInputInjector() = default;
LinuxInputInjector::~LinuxInputInjector() { stop(); }

bool LinuxInputInjector::start(int width, int height, std::string* error) {
  if (fd_ >= 0) return true;
  width_ = std::max(1, width);
  height_ = std::max(1, height);

  fd_ = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd_ < 0) {
    if (error) *error = "open /dev/uinput: " + std::string(std::strerror(errno));
    return false;
  }

  auto fail = [&](const char* operation) {
    if (error) {
      *error = std::string(operation) + ": " + std::strerror(errno);
    }
    stop();
    return false;
  };

  if (ioctl(fd_, UI_SET_EVBIT, EV_KEY) < 0) return fail("UI_SET_EVBIT EV_KEY");
  for (int code = KEY_ESC; code <= KEY_MICMUTE; ++code) {
    if (ioctl(fd_, UI_SET_KEYBIT, code) < 0) return fail("UI_SET_KEYBIT");
  }
  const int buttons[] = {BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, BTN_SIDE, BTN_EXTRA};
  for (int code : buttons) {
    if (ioctl(fd_, UI_SET_KEYBIT, code) < 0) return fail("UI_SET_KEYBIT mouse");
  }

  if (ioctl(fd_, UI_SET_EVBIT, EV_ABS) < 0) return fail("UI_SET_EVBIT EV_ABS");
  if (ioctl(fd_, UI_SET_ABSBIT, ABS_X) < 0) return fail("UI_SET_ABSBIT ABS_X");
  if (ioctl(fd_, UI_SET_ABSBIT, ABS_Y) < 0) return fail("UI_SET_ABSBIT ABS_Y");
  if (ioctl(fd_, UI_SET_EVBIT, EV_REL) < 0) return fail("UI_SET_EVBIT EV_REL");
  if (ioctl(fd_, UI_SET_RELBIT, REL_WHEEL) < 0) return fail("UI_SET_RELBIT REL_WHEEL");
  if (ioctl(fd_, UI_SET_RELBIT, REL_HWHEEL) < 0) return fail("UI_SET_RELBIT REL_HWHEEL");

  uinput_abs_setup abs_setup{};
  abs_setup.code = ABS_X;
  abs_setup.absinfo.minimum = 0;
  abs_setup.absinfo.maximum = width_ - 1;
  if (ioctl(fd_, UI_ABS_SETUP, &abs_setup) < 0) return fail("UI_ABS_SETUP ABS_X");
  abs_setup = uinput_abs_setup{};
  abs_setup.code = ABS_Y;
  abs_setup.absinfo.minimum = 0;
  abs_setup.absinfo.maximum = height_ - 1;
  if (ioctl(fd_, UI_ABS_SETUP, &abs_setup) < 0) return fail("UI_ABS_SETUP ABS_Y");

  uinput_setup setup{};
  std::snprintf(setup.name, UINPUT_MAX_NAME_SIZE, "AI Remote Control Input");
  setup.id.bustype = BUS_VIRTUAL;
  setup.id.vendor = 0x1d6b;
  setup.id.product = 0x0104;
  setup.id.version = 1;
  if (ioctl(fd_, UI_DEV_SETUP, &setup) < 0) return fail("UI_DEV_SETUP");
  if (ioctl(fd_, UI_DEV_CREATE) < 0) return fail("UI_DEV_CREATE");

  // 给桌面/libinput 一个发现新设备的机会，避免连接后的第一次点击丢失。
  usleep(100000);
  return true;
}

void LinuxInputInjector::stop() {
  if (fd_ >= 0) {
    ioctl(fd_, UI_DEV_DESTROY);
    close(fd_);
    fd_ = -1;
  }
}

bool LinuxInputInjector::emit(uint16_t type, uint16_t code, int32_t value) {
  if (fd_ < 0) return false;
  input_event event{};
  event.type = type;
  event.code = code;
  event.value = value;
  return write(fd_, &event, sizeof(event)) == static_cast<ssize_t>(sizeof(event));
}

bool LinuxInputInjector::sync() {
  return emit(EV_SYN, SYN_REPORT, 0);
}

void LinuxInputInjector::mouse_move(int x, int y) {
  x = std::clamp(x, 0, width_ - 1);
  y = std::clamp(y, 0, height_ - 1);
  emit(EV_ABS, ABS_X, x);
  emit(EV_ABS, ABS_Y, y);
  sync();
}

void LinuxInputInjector::mouse_button(int button, bool pressed, int x, int y) {
  const int code = qt_button_to_linux(button);
  if (code == 0) return;
  mouse_move(x, y);
  emit(EV_KEY, static_cast<uint16_t>(code), pressed ? 1 : 0);
  sync();
}

void LinuxInputInjector::mouse_wheel(int delta_x, int delta_y) {
  wheel_remainder_x_ += delta_x;
  wheel_remainder_y_ += delta_y;
  const int horizontal = wheel_remainder_x_ / 120;
  const int vertical = wheel_remainder_y_ / 120;
  wheel_remainder_x_ %= 120;
  wheel_remainder_y_ %= 120;
  if (horizontal != 0) emit(EV_REL, REL_HWHEEL, horizontal);
  if (vertical != 0) emit(EV_REL, REL_WHEEL, vertical);
  if (horizontal != 0 || vertical != 0) sync();
}

void LinuxInputInjector::key(int qt_key, int state) {
  const int code = qt_key_to_linux(qt_key);
  if (code == 0) return;
  emit(EV_KEY, static_cast<uint16_t>(code), std::clamp(state, 0, 2));
  sync();
}

}  // namespace rc
