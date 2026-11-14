# 远程画面全屏与远程操作功能更新

## 一、更新概述

本次更新让主控端（Windows Qt 客户端）在查看被控端（Ubuntu）远程画面时，可以进入全屏，并直接用本机鼠标、滚轮和键盘操作远程系统。输入复用现有会话与中继链路，不额外建立输入连接。

同时完成了 PipeWire 桌面采集走 xdg-desktop-portal、画面质量配置改为实际数据驱动、Qt6/Qt5 双版本兼容等配套改动。

## 二、功能说明

### 1. 远程画面全屏

- 显示页“远程画面”卡片右上角新增“全屏（F11）”按钮。
- 画面获得焦点后，按 `F11` 进入/退出全屏；全屏下按 `Esc` 退出。
- 全屏时画面作为无边框顶层窗口铺满屏幕，左上角显示“F11 / Esc 退出全屏”提示。
- 退出全屏后自动恢复到原父容器和原布局位置，侧栏、导航和状态栏保持原样。

### 2. 远程鼠标操作

- 鼠标移动实时发送到被控端。
- 支持左键、右键、中键、后退、前进按键。
- 支持垂直与水平滚轮。
- 鼠标坐标按实际视频帧显示区域换算，自动排除黑边（保持宽高比），并钳制在远程分辨率范围内，因此窗口缩放和全屏模式下定位都准确。

### 3. 远程键盘操作

- 支持字母、数字、空格、标点符号及 Shift 组合符号。
- 支持 Shift / Ctrl / Alt / Meta、方向键、Home / End / PageUp / PageDown、Esc / Tab / Backspace / Enter / Insert / Delete、F1–F24。
- 支持按键自动重复。
- 画面失去焦点、停止连接或退出全屏时，自动补发已按下按键和鼠标键的释放事件，避免远程端出现按键卡住。

### 4. 权限控制

- 现有“键盘”“鼠标”权限开关直接作用于输入发送：关闭后对应输入不会发往远程端。

### 5. 画面质量信息改为实际数据

- 删除了显示页中静态的“分辨率 / FPS / 质量”配置项。
- 画面按实际收到的视频帧尺寸等比显示，不再依赖预设值。

## 三、实现要点

### 主控端

| 项目 | 实现 |
| --- | --- |
| 画面控件 | `apps/qt/main_window.cpp` 中的 `VideoWidget`，承担显示、全屏切换和键鼠事件采集 |
| 全屏切换 | `setParent(nullptr, Qt::Window \| Qt::FramelessWindowHint)` + `showFullScreen()`；退出时恢复父窗口与布局索引 |
| 坐标映射 | 以 `QImage` 实际尺寸和 `Qt::KeepAspectRatio` 计算显示矩形，再线性映射到远程像素坐标 |
| 输入回调 | `setInputCallback()` 把 `rc::InputEvent` 交给主窗口，按权限开关过滤后送入后端 |
| UI 线程安全 | `BackendWorker::sendInput()` 为普通方法，只写入有界队列，不排队到被阻塞的工作线程 |
| 输入发送线程 | `ClientRunner` 内独立线程消费队列，`Connection::send()` 已有发送锁，可与视频、心跳并发 |
| 队列策略 | 最大 512 条；连续鼠标移动合并为最新位置，避免网络变慢后堆积过期操作 |

### 被控端

| 项目 | 实现 |
| --- | --- |
| 输入注入 | 新增 `core/input/linux_input_injector.{h,cpp}`，通过 Linux `uinput` 创建虚拟键鼠设备 |
| 兼容性 | 走内核输入子系统，同时适用于 Wayland 与 X11，不依赖 `xdotool` |
| 绝对坐标 | `EV_ABS / ABS_X / ABS_Y`，范围与采集分辨率一致 |
| 按键与滚轮 | `EV_KEY` 映射键盘和鼠标键；`EV_REL / REL_WHEEL / REL_HWHEEL` 处理滚轮，按 120 单位换算 |
| 设备生命周期 | 随采集流程启动；停止或析构时 `UI_DEV_DESTROY`，系统自动释放仍按下的键 |
| 键码映射 | Qt 键值到 Linux 键码逐项映射；字母键码在 Linux 中并不连续，已按表映射修正 |

### 协议与中继

- 复用已有 `MsgType::InputMouseMove / InputMouseButton / InputMouseWheel / InputKey`（0x20–0x24）。
- 二进制载荷：
  - 鼠标移动 10 字节：时间戳 + x/y + 保留位
  - 鼠标按键 12 字节：时间戳 + 按键 + 状态 + x/y + 保留位
  - 滚轮 8 字节：时间戳 + 水平/垂直增量
  - 键盘 12 字节：时间戳 + Qt 键值 + 状态（0 松开、1 按下、2 自动重复）+ 保留位
- 中继对带 `session_id` 的消息按会话转发，输入消息无需新增通道或修改转发逻辑。

## 四、PipeWire 采集配套改动

- 采集改为 `libportal + xdg-desktop-portal + PipeWire` 流程：
  1. 创建 screencast session（首次弹窗选屏）
  2. `xdp_session_open_pipewire_remote()` 获取 portal 专用 PipeWire fd
  3. `xdp_session_get_streams()` 获取精确 node_id
  4. `pw_context_connect_fd()` 直连并拉流
- restore token 持久化到 `$XDG_CACHE_HOME/ai_control_pc/screencast_restore_token`，二次启动免弹窗；token 失效时自动重新走弹窗。
- 构建依赖新增 `libportal`，CMake 增加 `pkg_check_modules(LIBPORTAL libportal)`。

## 五、构建与运行环境

- 桌面 UI 优先使用 Qt6，未找到时回退 Qt5。
- Ubuntu 需安装 `libportal-dev`、PipeWire 与 GLib/GIO 开发库。
- `uinput` 权限通过 udev 规则授予运行用户：

```text
KERNEL=="uinput", SUBSYSTEM=="misc", OWNER="lansnow", GROUP="lansnow", MODE="0600"
```

保存为 `/etc/udev/rules.d/60-ai-control-uinput.rules` 后执行：

```bash
sudo udevadm control --reload-rules
sudo udevadm trigger
```

## 六、验证结果

- Windows 主控端与 Ubuntu 被控端均构建通过。
- F11 进入全屏、Esc 退出全屏、退出后窗口布局恢复：通过。
- 远程画面持续接收，状态显示“传输中”。
- 在被控端终端内通过主控真实鼠标点击定位，并输入：

```bash
echo RC_INPUT_OK >/tmp/rc_input_ok
```

- 远程终端正确显示命令并返回提示符，`/tmp/rc_input_ok` 内容为 `RC_INPUT_OK`，覆盖鼠标点击、字母、大小写、符号、路径分隔符和回车。

## 七、涉及文件

新增：

- `core/input/linux_input_injector.h`
- `core/input/linux_input_injector.cpp`

修改：

- `apps/qt/main_window.cpp` / `main_window.h`
- `apps/qt/backend_worker.cpp` / `backend_worker.h`
- `apps/qt/main.cpp`
- `core/session/runner.cpp` / `runner.h`
- `core/capture/pipewire_capture.cpp` / `pipewire_capture.h`
- `CMakeLists.txt`

## 八、使用说明

1. 主控端连接被控端，等待画面出现。
2. 单击远程画面使其获得焦点。
3. 按 `F11` 或点击“全屏（F11）”进入全屏。
4. 直接使用鼠标和键盘操作远程系统。
5. 按 `Esc` 或 `F11` 退出全屏。
6. 如需临时禁用某类输入，在“权限”页关闭“键盘”或“鼠标”开关。

## 九、已知限制

- 键盘输入按 Qt 键码传递，暂不处理输入法组合文本（中文等 IME 候选）。
- 剪贴板同步、多显示器切换、远程分辨率变化后的坐标范围刷新尚未纳入本次更新。
- 被控端输入注入依赖 `/dev/uinput` 权限，未配置 udev 规则时鼠标键盘不可用。
