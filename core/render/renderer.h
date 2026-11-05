#pragma once

#include <cstdint>
#include <string>

namespace rc {

// 渲染器抽象接口。Windows（DXGI swapchain / GDI 兜底）与 Linux（SDL2）各有实现。
// 调用方通过该接口统一管理窗口生命周期与画面上屏，无需关心平台后端。
class IRenderer {
 public:
  virtual ~IRenderer() = default;

  virtual bool open(const std::string& title, int width, int height,
                    std::string* error) = 0;
  virtual void close() = 0;
  virtual bool alive() const = 0;

  // true 表示已具备 GPU 上屏能力（可吃 NV12 直出）。
  virtual bool gpu() const = 0;

  // 非阻塞处理窗口消息（拖拽/关闭），需在主循环里持续调用。
  virtual void pump() = 0;

  // BGRA 上屏（GDI/SDL 兜底路径）。
  virtual void present(const uint8_t* bgra, int width, int height, int stride) = 0;

  // NV12 直出：coded_height 为编码高度（可能含对齐行），visible_height 为可见高度。
  // 不支持 NV12 直出的实现返回 false（默认实现即如此），调用方应回退 BGRA。
  virtual bool present_nv12(const uint8_t* nv12, int width, int visible_height,
                            int coded_height, int stride) {
    (void)nv12;
    (void)width;
    (void)visible_height;
    (void)coded_height;
    (void)stride;
    return false;
  }

  virtual const char* name() const = 0;
};

}  // namespace rc
