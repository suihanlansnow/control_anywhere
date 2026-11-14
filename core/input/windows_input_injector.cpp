#include "input/windows_input_injector.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cstdint>

namespace rc {
namespace {

struct KeyMapping {
  WORD key = 0;
  bool extended = false;
};

KeyMapping qt_key_to_windows(int key) {
  if (key >= 'A' && key <= 'Z') return {static_cast<WORD>(key), false};
  if (key >= '0' && key <= '9') return {static_cast<WORD>(key), false};
  if (key >= 0x01000030 && key <= 0x01000047) {
    return {static_cast<WORD>(VK_F1 + key - 0x01000030), false};
  }

  switch (key) {
    case 0x01000000: return {VK_ESCAPE, false};
    case 0x01000001:
    case 0x01000002: return {VK_TAB, false};
    case 0x01000003: return {VK_BACK, false};
    case 0x01000004:
    case 0x01000005: return {VK_RETURN, false};
    case 0x01000006: return {VK_INSERT, true};
    case 0x01000007: return {VK_DELETE, true};
    case 0x01000008: return {VK_PAUSE, false};
    case 0x01000009: return {VK_SNAPSHOT, true};
    case 0x01000010: return {VK_HOME, true};
    case 0x01000011: return {VK_END, true};
    case 0x01000012: return {VK_LEFT, true};
    case 0x01000013: return {VK_UP, true};
    case 0x01000014: return {VK_RIGHT, true};
    case 0x01000015: return {VK_DOWN, true};
    case 0x01000016: return {VK_PRIOR, true};
    case 0x01000017: return {VK_NEXT, true};
    case 0x01000020: return {VK_SHIFT, false};
    case 0x01000021: return {VK_CONTROL, false};
    case 0x01000022: return {VK_LWIN, true};
    case 0x01000023: return {VK_MENU, false};
    case 0x01000024: return {VK_CAPITAL, false};
    case 0x01000025: return {VK_NUMLOCK, true};
    case 0x01000026: return {VK_SCROLL, false};
    case 0x01001103: return {VK_RMENU, true};
    case ' ': return {VK_SPACE, false};
    case '!': return {'1', false};
    case '@': return {'2', false};
    case '#': return {'3', false};
    case '$': return {'4', false};
    case '%': return {'5', false};
    case '^': return {'6', false};
    case '&': return {'7', false};
    case '*': return {'8', false};
    case '(': return {'9', false};
    case ')': return {'0', false};
    case ';':
    case ':': return {VK_OEM_1, false};
    case '=':
    case '+': return {VK_OEM_PLUS, false};
    case ',':
    case '<': return {VK_OEM_COMMA, false};
    case '-':
    case '_': return {VK_OEM_MINUS, false};
    case '.':
    case '>': return {VK_OEM_PERIOD, false};
    case '/':
    case '?': return {VK_OEM_2, false};
    case '`':
    case '~': return {VK_OEM_3, false};
    case '[':
    case '{': return {VK_OEM_4, false};
    case '\\':
    case '|': return {VK_OEM_5, false};
    case ']':
    case '}': return {VK_OEM_6, false};
    case '\'':
    case '"': return {VK_OEM_7, false};
    default: return {};
  }
}

struct MouseMapping {
  DWORD down = 0;
  DWORD up = 0;
  DWORD data = 0;
};

MouseMapping mouse_mapping(int button) {
  switch (button) {
    case 1: return {MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP, 0};
    case 2: return {MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP, 0};
    case 3: return {MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_MIDDLEUP, 0};
    case 4: return {MOUSEEVENTF_XDOWN, MOUSEEVENTF_XUP, XBUTTON1};
    case 5: return {MOUSEEVENTF_XDOWN, MOUSEEVENTF_XUP, XBUTTON2};
    default: return {};
  }
}

}  // namespace

WindowsInputInjector::~WindowsInputInjector() { stop(); }

bool WindowsInputInjector::start(int width, int height, std::string* error) {
  if (width <= 0 || height <= 0) {
    if (error) *error = "invalid capture size";
    return false;
  }
  width_ = width;
  height_ = height;
  return true;
}

void WindowsInputInjector::stop() {
  for (WORD key_code : pressed_keys_) send_key(key_code, false);
  pressed_keys_.clear();
  for (int button : pressed_buttons_) {
    const MouseMapping mapping = mouse_mapping(button);
    if (mapping.up != 0) send_mouse(mapping.up, mapping.data);
  }
  pressed_buttons_.clear();
}

void WindowsInputInjector::send_mouse(DWORD flags, DWORD data, int x, int y) {
  INPUT input{};
  input.type = INPUT_MOUSE;
  input.mi.dwFlags = flags;
  input.mi.mouseData = data;
  if ((flags & MOUSEEVENTF_MOVE) != 0) {
    x = std::clamp(x, 0, width_ - 1);
    y = std::clamp(y, 0, height_ - 1);
    const int max_x = std::max(1, width_ - 1);
    const int max_y = std::max(1, height_ - 1);
    input.mi.dx = static_cast<LONG>((static_cast<int64_t>(x) * 65535 + max_x / 2) / max_x);
    input.mi.dy = static_cast<LONG>((static_cast<int64_t>(y) * 65535 + max_y / 2) / max_y);
  }
  SendInput(1, &input, sizeof(input));
}

void WindowsInputInjector::send_key(WORD key_code, bool pressed) {
  INPUT input{};
  input.type = INPUT_KEYBOARD;
  input.ki.wVk = key_code;
  switch (key_code) {
    case VK_RMENU:
    case VK_RCONTROL:
    case VK_INSERT:
    case VK_DELETE:
    case VK_HOME:
    case VK_END:
    case VK_PRIOR:
    case VK_NEXT:
    case VK_LEFT:
    case VK_UP:
    case VK_RIGHT:
    case VK_DOWN:
    case VK_SNAPSHOT:
    case VK_NUMLOCK:
    case VK_LWIN:
    case VK_RWIN:
      input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
      break;
    default:
      break;
  }
  if (!pressed) input.ki.dwFlags |= KEYEVENTF_KEYUP;
  SendInput(1, &input, sizeof(input));
}

void WindowsInputInjector::mouse_move(int x, int y) {
  send_mouse(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE, 0, x, y);
}

void WindowsInputInjector::mouse_button(int button, bool pressed, int x, int y) {
  const MouseMapping mapping = mouse_mapping(button);
  if (mapping.down == 0) return;
  mouse_move(x, y);
  if (pressed) pressed_buttons_.insert(button);
  else pressed_buttons_.erase(button);
  send_mouse(pressed ? mapping.down : mapping.up, mapping.data);
}

void WindowsInputInjector::mouse_wheel(int delta_x, int delta_y) {
  if (delta_x != 0) send_mouse(MOUSEEVENTF_HWHEEL, static_cast<DWORD>(delta_x));
  if (delta_y != 0) send_mouse(MOUSEEVENTF_WHEEL, static_cast<DWORD>(delta_y));
}

void WindowsInputInjector::key(int qt_key, int state) {
  const KeyMapping mapping = qt_key_to_windows(qt_key);
  if (mapping.key == 0) return;
  const bool pressed = state != 0;
  if (pressed) pressed_keys_.insert(mapping.key);
  else pressed_keys_.erase(mapping.key);
  send_key(mapping.key, pressed);
}

}  // namespace rc
