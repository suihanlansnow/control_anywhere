# Wayland 屏幕抓取方案全景调研

> 目的：梳理本项目 Wayland 远程桌面画面全黑问题的解决过程中涉及的所有方案，对比异同，并给出业界完整方案图谱。

---

## 一、本项目走过的方案

### 方案总览

| # | 方案 | 会话要求 | 状态 | 核心路径 |
|---|------|---------|------|---------|
| A | X11 `XShmGetImage` | **必须 X11** | ✅ 工作正常 | X 协议 → 共享内存段 → `XShmGetImage` 拷贝 BGRA |
| B | **xdg-desktop-portal + PipeWire** | **Wayland 或 X11 均可** | 🔧 格式协商中 | D-Bus 授权 → 拿 PipeWire node_id → `pw_stream` 拉帧 |

### 方案 B 内部的子尝试（同一个大方向下踩过的坑）

| 子尝试 | 变更 | 结果 | 根因分析 |
|--------|------|------|---------|
| B-1 | `pw_stream_connect` 不传 params | `no target node available` | WirePlumber 不知道怎么 link（缺格式 hint） |
| B-2 | 传 EnumFormat params（BGRA） + direct target | ✅ 找到节点 → 收到 Props 回调 → **Format=NULL** | 节点找到但协商不完整 |
| B-3 | EnumFormat + AUTOCONNECT | ✅ 找到节点 + 收到 Props → **Format=NULL 循环** | WirePlumber link 成功但格式 pod 为 nil |
| B-4 | Portal 后 sleep 2s 再连 | ❌ 节点超时销毁 | Portal 节点寿命 ~10-20s，等不起 |
| B-5 | 去掉 sleep + EnumFormat + direct target | ✅ 收到 Props → **收到 Format pod** → `spa_format_video_raw_parse` 返回 EINVAL | 格式 pod 存在但字段布局不匹配 BGRA |
| B-6 | pw_stream_properties 里设 `PW_KEY_TARGET_OBJECT` | ❌ `no target node available` | PipeWire 1.0.5 对此支持不完整 |

### 关键认知收获

1. **`params` 是必须的** — 不传 params WirePlumber 会"迷失方向"（no target）；传了 EnumFormat 它才有东西做 link 决策。
2. **Portal 节点寿命极短（10-20s）** — 从 portal `Start` 成功返回 node_id 那一刻起就倒计时，任何人为 sleep 都不可接受。
3. **Format=NULL 是 WirePlumber/Portal 协商中的一个合法中间态** — 我们在 B-2/B-3 里看到的循环重试后 unconnected 是正常的超时行为。
4. **`spa_format_video_raw_parse` EINVAL** — 格式 pod 确实存在，但声明的 BGRA 跟服务端（Portal/Screencast 节点）的实际输出格式字段有差异（可能 stride/fields_aligned 等字段值不符合 parse 预期）。
5. **AUTOCONNECT vs direct target** — direct target + EnumFormat 能"更快"找到节点；AUTOCONNECT 让 WirePlumber 有更多自主性但也更慢。

### 方案 B 当前卡点 & 出路

最新日志表现：

```
Format pod parse_object r=0 mediaType=1 mediaSubtype=1
Video=1 Audio=2 raw=1
spa_format_video_raw_parse failed r=4 (EINVAL)
→ 发送 BGRA Format 请求 → 再次 Format=NULL → 循环 → unconnected
```

Format pod **存在**且 mediaType=Video/raw=1 正确，但 `spa_format_video_raw_parse` 报 EINVAL。根本原因是：声明的 BGRA（动态尺寸）跟服务端实际协商的尺寸/stride/像素布局不完全一致。

三个可能的出路：

| 出路 | 思路 | 工作量 | 风险 |
|------|------|--------|------|
| **Z1. 调试 parse 失败原因** | 打印 `spa_pod_contents_of` 逐项解析服务端 Format pod，看哪些字段缺失/不匹配 | 中 | 字段太多，Pod 结构复杂，可能花 2-3 天 |
| **Z2. 不 parse，直接信任服务端** | param_changed 收到 Format pod 后，**接受它而不是覆盖**。`spa_pod_contents_of` 手动提取 `SPA_FORMAT_VIDEO_size` 宽高，然后配置 Buffers | 中 | 需要完全理解服务端格式；但一旦通了就是标准路径 |
| **Z3. 放弃 parse 函数，自己走 SPA Pod 手动消费** | 用 `spa_pod_parse_object` + `SPA_POD_OPT_*` 逐项 read Format pod，然后按字段值配置 Buffers | 中 | 同样需要理解 pod，但比 parse 函数更可控 |

**推荐 Z2** — 核心洞察：代码不应该"告诉服务端我要什么格式"，而是应该"听服务端告诉我它会出什么格式"然后适配。这是 PipeWire 协商模型的精髓：consumer 说"我能接受什么"，producer 决定"我出什么"，双方都同意了才算成。之前一直在 consumer 侧硬推 BGRA，实际上应该先等 producer 出 Format，然后 **接受它**（哪怕是 NV12/YUV420），后续再做色彩空间转换。

---

## 二、Linux Wayland 屏幕抓取完整方案全景图

### 按协议栈分层

```
┌───────────────────────────────────────────────────────────┐
│                    应用层（我们的代码）                      │
├───────────────────────────────────────────────────────────┤
│                                                           │
│   ┌─────────────┐  ┌──────────────┐  ┌────────────────┐   │
│   │  X11/XShm   │  │ Wayland Port │  │ Wayland Native  │   │
│   │  (X11 only) │  │  (D-Bus)     │  │ (KMS/DMABUF)   │   │
│   └──────┬──────┘  └──────┬───────┘  └───────┬────────┘   │
│          │                │                   │            │
│   ┌──────▼────────────────▼──────────────────▼────────┐   │
│   │              PipeWire / SPA framework              │   │
│   │   (pw_stream, spa_pod_builder, spa_format_*)       │   │
│   └──────────────────────────────────────────────────┘   │
│                                                           │
├───────────────────────────────────────────────────────────┤
│                    系统依赖层                              │
│   xdg-desktop-portal + portal backend (gnome/kde/wlr)     │
│   wireplumber / pipewire                                  │
│   KMS/DRM / GPU Driver (i915, amdgpu, nvidia)             │
└───────────────────────────────────────────────────────────┘
```

### 六大方案对比矩阵

| 方案 | 技术路径 | 会话兼容 | 权限模型 | 性能 | 输出格式 | 实现复杂度 | 依赖 |
|------|---------|---------|---------|------|---------|-----------|------|
| **①X11 XShm** | X11 MIT-SHM 扩展 → `XShmGetImage` | 仅 X11；Wayland 下 XWayland 只能抓到自己窗口 | 无需授权（X11 全局信任模型） | ⭐⭐⭐⭐⭐ 零额外延迟 | BGRA (CPU) | ⭐ 极低（~100 行） | libX11 + libXext + libXshm |
| **②Portal + PipeWire** | D-Bus `org.freedesktop.portal.ScreenCast` → 三步授权 → 拿 node_id → `pw_stream` 拉帧 | Wayland ✅ / X11 ✅ | 用户 GUI 弹窗授权（可 dconf 自动） | ⭐⭐⭐ SPA 零拷贝路径（DMA-BUF），但 portal 有 round-trip | 一般输出 BGRA；可用 SPA 直接读 DMA-BUF | ⭐⭐⭐⭐⭐ 极高（1500+ 行） | libpipewire-0.3 + glib-2.0 + gio-2.0 + portal backend |
| **③Wayland Native Protocol** | Wayland 原生协议 `ext-image-copy-capture-v1` 或旧版 `zwlr-screencopy-v1` | Wayland only；**仅限 wlroots 系 compositor**（Sway/Hyprland/wlroots） | 首次使用需 `wlr-chooser`/`slurp` 选 output | ⭐⭐⭐⭐⭐ 原生 DMA-BUF 零拷贝 | DMA-BUF（GPU）→ 零拷贝导入 GL/Vulkan | ⭐⭐⭐⭐ 高 | wayland-client + wayland-protocols + 具体 protocol xml |
| **④KMS/DRM (headless)** | 直接 mmap `/dev/dri/card0` 读 framebuffer，或 DRM Atomic commit + eventfd poll | Wayland + X11 都行；**必须 root 或 CAP_SYS_ADMIN** | root 权限 | ⭐⭐⭐⭐⭐ 最快，直接读 HW framebuffer | CMA/UMEM（CPU）或 DMA-BUF | ⭐⭐⭐⭐ 中高 | libdrm（+ libdrm-intel/amdgpu/tegra 分 vendor） |
| **⑤NVIDIA NVFBC** | NVIDIA 专有 NVFBC API（`NvFBCToSys` / `NvFBCToDx11`） | 仅 X11；Wayland 下不可用 | 无授权 | ⭐⭐⭐⭐⭐ 专用 GPU capture engine | CUDA / system memory | ⭐⭐⭐ 中 | CUDA + nvidia-driver + nvidia-patch（GeForce 需 patch） |
| **⑥GPU 厂商扩展** | Intel KMS + i915 GEM / AMD KMS + amdgpu GEM | Wayland + X11 | root 或 DRM render 权限 | ⭐⭐⭐⭐⭐ | DMA-BUF / GEM mmap | ⭐⭐⭐⭐⭐ 极高（每个 GPU vendor 不同） | libdrm + vendor 特定 headers |

---

## 三、各方案详解

### 方案① X11 XShm（本项目已实现）

```c
// 核心就这几行：
XShmGetImage(display, root_window, &ximage, x, y);
// ximage->data 里就是 BGRA/BGRX 像素
```

- **优点**：实现极简、零延迟（直接从 X server 的共享内存段读）、无权限弹窗
- **致命缺点**：Wayland 下 XWayland 每个 client 是独立的嵌套 X server，XShm 只能看到自己的窗口

### 方案② Portal + PipeWire（本项目正在实现）

```
D-Bus 调用序列：
  1. CreateSession  → 返回 session_handle
  2. SelectSources  (session_handle, options) → 弹窗让用户选屏幕
  3. Start          (session_handle, options) → 返回 PipeWire node_id
       ↓
  PipeWire 侧：
  4. pw_thread_loop_new + pw_context_new + pw_context_connect
  5. pw_stream_new + pw_stream_connect(direct target=node_id, EnumFormat=BGRA)
  6. WirePlumber link 完成 → param_changed 回调收到 Format pod
  7. pw_process_cb 从 SPA buffers 拷贝 BGRA
```

- **优点**：GNOME/KDE/wlroots 全兼容；是 Chrome/WebRTC/OBS 同款技术栈
- **缺点**：实现复杂度最高（D-Bus + GLib main loop + PipeWire SPA 三层 API 叠加）；portal backend 必须跟 compositor 匹配；D-Bus 弹窗可能 hang；节点寿命极短

### 方案③ Wayland Native Protocol（第三方库推荐）

这是 **wlroots 系 compositor（Sway/Hyprland/wlroots）的"更原生"替代方案**：

| 协议 | 状态 | 输出目标 | 窗口级捕获 |
|------|------|---------|-----------|
| `ext-image-copy-capture-v1` | ✅ **当前标准** | Output / Toplevel | ✅ 支持 |
| `zwlr-screencopy-v1` | ⚠️ 已过时，被 ext-image-copy-capture 取代 | 仅 Output | ❌ 不支持 |

**主流实现库**：
- **wlr-capture**（Rust）— 自动 fallback 两个协议，零拷贝 DMA-BUF 路径，被 grim/satty/wf-recorder 底层使用
- **ashpd**（Rust）— GNOME/KDE/Sway 都兼容的高层 portal 封装
- **wayland-client + 手写 protocol**（C/C++，需要手写 xml→code 生成）

**为什么我们没选这个？** — **不支持 GNOME！** Mutter（GNOME compositor）**不实现** `ext-image-copy-capture-v1`，只走 portal 路径。所以这个方案对 GNOME 用户完全无效。

### 方案④ KMS/DRM Headless Capture

```c
// mmap framebuffer:
int fd = open("/dev/dri/card0", O_RDWR);
struct drm_mode_fb_cmd fb = {};
ioctl(fd, DRM_IOCTL_MODE_GETFB, &fb);
void *pixels = mmap(NULL, fb.pitches[0] * fb.height, PROT_READ, MAP_PRIVATE, fd, fb.offsets[0]);
```

**实际使用案例**：
- **Sunshine**（v0.23+）— Wayland 下自动切 KMS backend，`sunshine --capture kms`
- **gpu-screen-recorder**（Rust）— headless Wayland screen recorder
- **ffmpeg -f kmsgrab**（`ffmpeg -f kmsgrab -i - -vf 'hwdownload,format=bgrx' output.mp4`）

**致命问题**：**必须 root**，而且 KMS 是显示控制器层面的读取 — 如果 compositor 在 GPU 上做了合成，读到的可能是黑的或者合成后的数据。多 GPU 笔记本（Intel iGPU + NVIDIA dGPU）上 card0/card1 选择也是坑。

### 方案⑤ NVIDIA NVFBC

```c
NvFBCCreate(..., &handle);
NvFBCGetNextFrame(handle, &frameInfo, &capturedFrame);
// frameInfo.lpGpuCpuIpcResource 或 lpSysMem 里有像素
```

**限制**：
- GeForce 卡需要 `nvidia-patch` 打补丁（NVFBC 官方对 GeForce 有限制）
- 仅 **X11** 可用，Wayland 下 NVIDIA 驱动**不提供 NVFBC**（这是 NVIDIA 的锅）
- 仅 NVIDIA GPU

### 方案⑥ GPU Vendor 特定扩展

每个 GPU vendor 有自己的 DRM 扩展路径读 framebuffer：
- **Intel i915 GEM**：`DRM_IOCTL_I915_GEM_MMAP_GTT` mmap aperture（但 GTT 大小有限，4K 屏可能不够）
- **AMD amdgpu GEM**：`DRM_IOCTL_AMDGPU_GEM_CREATE + MMAP`
- **Mali/Adreno 等移动 GPU**：各有各自的 vendor ioctl

**缺点**：不可移植，每个 vendor 一套，而且内核升级可能 break。

---

## 四、方案共同点 vs 不同点

### 共同点（所有方案最终都在做的事）

| 抽象层 | 具体体现 |
|--------|---------|
| **1. 拿到 frame buffer** | 不管从 XShm / PipeWire / KMS mmap / NVFBC / DMA-BUF，最终都要把显存里的像素搬到用户态 |
| **2. 格式协商** | 都有"声明我要什么格式 ↔ 服务端告诉你实际是什么格式"（X11 的 visual/colormap；PipeWire 的 SPA Format pod；Wayland 的 pixel_format；KMS 的 DRM framebuffer ioctl） |
| **3. 同步/锁** | X11 的 XShmCompletionEvent；PipeWire 的 pw_buffer + spa_chunk；Wayland 的 fence + explicit sync；KMS 的 pageflip event |
| **4. 权限门禁** | X11 无；Portal/D-Bus 有弹窗；KMS 需要 CAP_SYS_ADMIN；NVFBC 依赖 NVIDIA 驱动配置 |
| **5. 零拷贝理想** | DMA-BUF / GEM / NVFBC / PipeWire SPA buffers 都宣称"零拷贝"，但 CPU 消费时仍需 readback |

### 核心差异

| 维度 | X11 XShm | Portal+PipeWire | Wayland Native | KMS/DRM |
|------|---------|-----------------|---------------|---------|
| **谁来合成画面** | X Server | Compositor (Mutter/KWin/wlroots) | Compositor | 完全绕过 compositor 直接读硬件 |
| **跨会话兼容** | 仅 X11 | Wayland + X11 | 仅 Wayland | 都行但需 root |
| **窗口级捕获** | ✅ `XGetWindowAttributes` 能枚举所有窗口 | ⚠️ Portal 3 支持；Portal 2 仅屏幕 | ✅ ext-image-copy-capture 支持 toplevel | ❌ 只能抓整个输出 |
| **多 monitor 处理** | ✅ Xinerama/RANDR | ✅ Portal 让用户选 | ✅ 枚举 wl_output | ⚠️ 多个 card 要分别读 |
| **实现语言友好度** | C / C++ / Rust 都好 | C + GLib + gio（C++ 能 include 但 GLib 是 C API，vtables 容易错）| C / Rust（wayland-client 有完整 Rust 绑定）| C / Rust（libdrm C API）|
| **实时性** | 最好（<1ms 延迟） | 中等（D-Bus round-trip + PipeWire scheduling） | 好（跟 compositor 的 event-driven） | 视 pageflip 而定 |

---

## 五、业界现成库实现

### 高层封装（直接提供"抓帧" API）

| 库 | 语言 | 路径 | 适用场景 |
|----|------|------|---------|
| **libportal** (C) | C | Portal + PipeWire 的低层 D-Bus 封装 | 想走 portal 但不想自己写 D-Bus 循环 |
| **ashpd** (Rust) | Rust | Portal + PipeWire 的高级 Rust wrapper | GNOME/KDE/Sway 全兼容 |
| **wlr-capture** (Rust) | Rust | Wayland Native (ext-image-copy-capture) + screencopy | 仅限 wlroots 系（Sway/Hyprland） |
| **miniscreenshot-desktop** (Rust) | Rust | 自动选 Wayland → X11 → Portal | 截图场景 |
| **PipeWire Example** (C) | C | 官方 `pw-video-capture.c` | 学习 pw_stream 的最权威资料 |

### 完整应用（内部已经实现了抓取管线）

| 应用 | 抓取路径 | 为什么能工作 |
|------|---------|-------------|
| **OBS Studio** | Wayland 下用 Portal+PipeWire source（选 "Screen Capture (PipeWire)"）| 完整实现了 D-Bus portal + pw_stream + SPA 路径 |
| **Chrome/WebRTC** | `chrome://flags/#enable-webrtc-pipewire-capturer` → Portal+PipeWire | Google Chrome 官方 SPA 实现 |
| **Sunshine** | 自动适配 KMS / Portal / wlr-screencopy / X11 / NVFBC | 5 种 fallback，Linux 下全覆盖 |
| **grim** | Wayland: wlr-screencopy / X11: XShm | 极简实现，仅截图 |
| **wf-recorder** | Wayland Native ext-image-copy-capture | wlroots 系录屏 |
| **gpu-screen-recorder** | Wayland Native + KMS fallback + portal fallback | headless 友好 |
| **gnome-remote-desktop** | Portal + PipeWire | GNOME 官方远程桌面，本项目 dconf 自动授权就是参考它的做法 |

### 现成库的"缺口"

**没有一个 C++ 原生库同时支持 GNOME + Sway + X11。** 这就是为什么被迫自己写：
- Rust 的 `wlr-capture` **不支持 GNOME**（Mutter 没有 screencopy）
- `ashpd` **是 Rust**，本项目是 C++
- 直接调用 libportal **还是要自己处理 PipeWire**
- libportal + libpipewire 组合起来就是现在在做的事

---

## 六、本项目架构建议

对于本项目（跨会话远程桌面），最优策略是 **分层 fallback**：

```cpp
capture_factory::create("auto") {
  if (session_is_wayland() && have_portal_backend())
    return new PipewireCapture();   // 方案 B: Portal + PipeWire
  else if (session_is_x11())
    return new X11Capture();        // 方案 A: XShm
  else if (session_is_wayland() && compositor_is_wlroots())
    // 可以加方案 C 的 future work
  else
    return new SyntheticCapture();  // 占位
}
```

**方案 B 修好后**，就能同时覆盖 GNOME（Portal）、KDE（Portal）、Sway/Hyprland（Portal + 原生协议 future）和 X11。

### 传输层架构建议（附加分析）

本地抓取完成后，统一编码为 H264，传输层可对比：

| 维度 | 当前方案（裸 TCP + 自定义 Relay） | WebRTC (SCTP-over-UDP) |
|------|--------------------------------|----------------------|
| 队头阻塞 | TCP 有队头阻塞，丢包影响全流 | SCTP per-stream，互不影响 |
| 拥塞控制 | TCP CUBIC 保守 | GCC/BBR 更激进，适合实时视频 |
| FEC/NACK | 无 | 内置 RTX + FEC 恢复丢包 |
| NAT 穿透 | 需要 STUN/TURN 自建 | 内置 ICE + STUN + TURN |
| 延迟 | 取决于 TCP 参数 | TURN relay 也能 <50ms |
| 实现复杂度 | 低（已有） | 中（libdatachannel 封装好） |
| 浏览器兼容 | 无 | 直接可在浏览器中作为 client |

**建议**：短期保持裸 TCP（已实现可用），中期切换 libdatachannel（C++ WebRTC wrapper）做传输层，上层保持 H264 编码输出不变。
