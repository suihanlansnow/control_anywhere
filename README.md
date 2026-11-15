# ai_control_pc — 远程桌面控制系统

一套跨平台的远程桌面与远程控制方案，支持 Windows / Linux 被控端，以及 Qt 桌面端、Web 浏览器和 Android 手机三种主控端。所有会话共用同一套中继协议与 TCP 通道，不额外建立输入连接。

## 架构总览

```
┌──────────────┐       ┌──────────────┐       ┌──────────────┐
│  主控端       │ ────► │  rc-relay    │ ◄──── │  被控端       │
│ (Qt/Web/安卓)│       │  中继服务器    │       │ (Win/Linux)  │
└──────────────┘       └──────────────┘       └──────────────┘
       视频/输入 ◄──────── 转发 ────────►  采集/编码/注入
```

- **中继** `rc-relay`：TCP 服务，负责设备注册、会话建立、鉴权挑战和消息转发，默认端口 `21116`。
- **被控端** `rc-cli host`：采集桌面 → H.264/JPEG 编码 → 推流；接收输入事件并注入系统。
- **主控端**：
  - `rc-cli connect` / `rc-app`（Qt）：桌面端查看与键鼠控制。
  - `rc-web`：HTTP 网关，把 H.264 转 MJPEG 推给浏览器，支持键鼠输入。
  - `apps/android`：原生 Android 客户端，MediaCodec 硬解 H.264，触控/滚轮/键盘远程操控。

## 目录结构

```
.
├── apps/
│   ├── android/       # Android 主控客户端（Java + MediaCodec）
│   ├── cli/           # rc-cli 命令行（host / connect / selftest）
│   └── qt/            # rc-app Qt 桌面主控端
├── core/
│   ├── base/          # 日志、JSON、时钟
│   ├── crypto/        # HMAC-SHA256 鉴权
│   ├── net/           # TCP Socket、连接、帧头协议
│   ├── protocol/      # 消息类型、视频载荷
│   ├── session/       # 会话状态机、HostRunner / ClientRunner
│   ├── capture/       # 屏幕采集（DXGI / X11 / PipeWire）
│   ├── codec/         # H.264 / JPEG 编解码
│   ├── input/         # 键鼠注入（Windows / Linux uinput）
│   └── render/        # 画面渲染（Win32 / SDL）
├── server/
│   ├── relay/         # rc-relay 中继服务器
│   └── web/           # rc-web HTTP 预览与控制网关
├── proto/             # 协议定义（messages.yaml）
└── res/               # 设计稿、调研文档
```

## 构建

### 桌面端（rc-relay / rc-cli / rc-app / rc-web）

依赖：CMake 3.20+、C++20 编译器、FFmpeg、SDL2（Linux）、Qt5/Qt6（可选，构建 rc-app）。

```bash
cmake -S . -B build
cmake --build build --config Release
```

Windows 产物示例：`build/Release/rc-cli.exe`、`rc-relay.exe`、`rc-app.exe`、`rc-web.exe`。

### Android 客户端

```bash
cd apps/android
./gradlew assembleDebug
# 产物: app/build/outputs/apk/debug/app-debug.apk
```

> 需 JDK 17+ 与 Android SDK（compileSdk 34, minSdk 23）。若离线环境无 gradle 网络依赖，可直接用本地 gradle 发行版 `gradle --offline assembleDebug`。

## 运行

### 1. 启动中继

```bash
rc-relay --port 21116
```

### 2. Windows / Ubuntu 桌面端

启动 `rc-app` 即可，**被控端会自动开启**，无需点击任何按钮：

- 首次启动自动生成唯一设备 ID（`win-<主机名>` / `linux-<主机名>`）和 8 位连接密码，并保存。
- 首页「本机（被控端 · 已自动开启）」卡片直接显示这两个值，并提供复制 / 重置。
- 中继不可达时自动重试，恢复后自动重新注册。
- 首页提供「允许远程键鼠操作」开关：取消勾选即进入**仅观看模式**，会话保持连接、
  画面继续推送，但远端的鼠标与键盘事件会被直接丢弃；重新勾选立即恢复操控。
- 同机可同时作为主控端连接其他设备（被控端与主控端各占独立线程）。

> 也可继续用命令行方式启动被控端（适合无图形界面的服务器）：
> `rc-cli host --id win-host --password 123456 --relay 127.0.0.1:21116 --frames 0`
> `--frames 0` 表示持续推流；默认 `--frames 60` 会在几秒后结束会话，仅用于自测。

### 3. 主控端连接

远端用户只需要拿到被控端首页显示的 **设备 ID** 与 **密码**（外加中继地址）。

**桌面 Qt 应用：**

在「连接」页填写中继地址、被控端设备 ID、被控端密码，点击「连接远程桌面」。

**Web 网关：**

```bash
rc-web --http-port 8080 --relay 127.0.0.1:21116
# 浏览器打开 http://<本机IP>:8080/，在页面中填写中继地址、被控端设备 ID、密码后连接
```

> `--target` / `--password` 可选；不传时网关照常启动，等待浏览器提交被控端信息。
> 页面右上角「切换设备」会先断开会话，再回到填写页。
> 相关接口：`POST /api/connect {relay,target,password}`、`POST /api/disconnect`。

**Android：**

安装 `app-debug.apk`，在连接页填写中继地址、被控端设备 ID、密码后连接，即可触控操作远程桌面。

**命令行：**

```bash
rc-cli connect win-host --password 123456 --relay 127.0.0.1:21116
```

## 协议要点

中继帧头 24 字节（小端）：

| 偏移 | 字段 | 说明 |
|------|------|------|
| 0 | `u8 ver` | 协议版本，当前 1 |
| 1 | `u8 type` | 消息类型 |
| 2 | `u8 flags` | 标志位 |
| 3 | `u8 reserved` | 保留 |
| 4 | `u32 msg_id` | 消息 ID |
| 8 | `u64 session_id` | 会话 ID |
| 16 | `u32 seq` | 序列号 |
| 20 | `u32 payload_len` | 载荷长度（≤1MiB） |

主要消息类型：`Register 0x03`、`ConnectReq 0x05`、`ConnectRsp 0x06`、`Heartbeat 0x0A`、`AuthChallenge 0x13`、`AuthResponse 0x14`、`AuthResult 0x15`、`SessionClose 0x16`、`MouseMove 0x20`、`MouseButton 0x21`、`MouseWheel 0x22`、`Key 0x23`、`VideoFrame 0x31`。

视频载荷头 32 字节：`capture_ts_us@0`、`encode_ts_us@8`、`frame_id@16`、`width u16@20`、`height u16@22`、`codec u8@24`（1=H.264）、`keyframe@25`、`data_len u32@28`，随后为 Annex-B H.264 数据。

## 核心特性

- **复用单通道**：视频与输入走同一 TCP 会话，中继按 `session_id` 转发。
- **HMAC-SHA256 鉴权**：中继下发挑战，被控端用密码生成 proof，主控端回传验证。
- **跨平台采集**：Windows DXGI；Linux X11 或 PipeWire（xdg-desktop-portal）。
- **跨平台注入**：Windows `SendInput`；Linux `/dev/uinput`（支持 Wayland 与 X11）。
- **硬件解码**：Android 用 MediaCodec 解码到 Surface，零拷贝上屏。
- **Web 网关节能**：无观看者时降到 2fps，无活动 15 秒后暂停会话；静止帧复用 JPEG 跳过编码。

## 已知限制

- 键盘按 Qt 键码传递，暂不支持输入法组合文本（中文等 IME 候选）。
- Android 客户端暂无文本输入协议，中文输入需依赖被控端已有输入法。
- 多显示器切换、远程分辨率变化后的坐标范围自动刷新尚未实现。
- Linux 输入注入依赖 `/dev/uinput` 权限，需配置 udev 规则。

## 文档

- [远程画面全屏与远程操作更新](res/REMOTE-CONTROL-UPDATE.md)
- [Wayland 屏幕抓取方案调研](res/WAYLAND-CAPTURE-SURVEY.md)
