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
# 明文 TCP（内网/本机自测）
rc-relay --port 21116

# TLS 加密（推荐公网部署；--tls-cert/--tls-key 为 PEM 文件）
rc-relay --port 21116 --tls-cert relay.pem --tls-key relay-key.pem
```

启用 TLS 后，被控端与主控端都要加 `--tls`，并用以下三种方式之一校验证书：

```bash
# 1) 固定证书 SHA-256 指纹（自签证书最常见）
rc-cli host ... --tls --tls-fingerprint 9a74fece...25e13
# 2) 提供 CA 证书链
rc-cli host ... --tls --tls-ca ca.pem
# 3) 仅加密、不校验（只建议本机/测试）
rc-cli host ... --tls --tls-insecure
```

Qt 端在「网络」页勾选「TLS 加密」并填入中继证书指纹即可（留空表示仅加密不校验）。
指纹为中继证书 DER 的 SHA-256 小写十六进制，可从中继日志或客户端首次连接日志中获取。

自签证书可用 mbed TLS 自带的 `gen_key` / `cert_write` 生成，或使用任意标准 X.509 工具。

#### 设备身份与防顶号

被控端首次启动会在本机生成 EC P-256 私钥并保存：

- Windows：`%APPDATA%\rc-desktop\identity.pem`
- Linux：`$XDG_CONFIG_HOME/rc-desktop/identity.pem`（缺省 `~/.config`）
- CLI 可用 `--identity <文件>` 指定其他位置。

注册时中继下发一次性 nonce，被控端用私钥对 `rc-register-v1|<设备号>|<nonce>` 签名；
中继校验通过后把公钥**固定**到该设备号，之后只有同一密钥的重连才会被接受，
其他密钥会收到 `device_id_taken`。同一密钥重连会替换旧连接，用于断线后快速重注册。

固定关系默认写入 `relay-devices.json`（当前工作目录），可用：

```bash
rc-relay --port 21116 --device-db /var/lib/rc/relay-devices.json   # 自定义位置
rc-relay --port 21116 --no-device-db                                # 只在内存固定
```

> 主控端不参与设备号固定，注册更轻量；设备身份只保护可被连接的被控端。

### 2. Windows / Ubuntu 桌面端

启动 `rc-app` 即可，**被控端会自动开启**，无需点击任何按钮：

- 首次启动自动生成唯一设备 ID（`win-<主机名>` / `linux-<主机名>`）和 8 位连接密码，并保存。
- 首页「本机（被控端 · 已自动开启）」卡片直接显示这两个值，并提供复制 / 重置。
- 中继不可达时自动重试，恢复后自动重新注册。
- 首页提供「允许远程键鼠操作」开关：取消勾选即进入**仅观看模式**，会话保持连接、
  画面继续推送，但远端的鼠标与键盘事件会被直接丢弃；重新勾选立即恢复操控。
- 同机可同时作为主控端连接其他设备（被控端与主控端各占独立线程）。
- 「网络」页的连接超时滑块（3–30 秒）与 `TCP_NODELAY` 开关会在建立连接时生效；
  「自动重连」开关控制主控端断线后是否自动重试（密码错误不会重试）。
- 编码固定为 H.264（优先硬件编码，失败自动回退软件编码），客户端内置 H.264 解码器。
- 「关于」页的「检查更新」会打开项目的 GitHub Releases 页面。

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
# 启动日志会打印带令牌的地址，例如：
#   网页控制台 http://127.0.0.1:8080/?token=3f2a...
# 浏览器首次打开该地址后写入 HttpOnly Cookie，后续页面/流/接口自动携带认证。
```

> 默认只监听 `127.0.0.1`；需要局域网访问时显式加 `--listen 0.0.0.0`。
> `--token <令牌>` 可固定访问令牌；省略时每次启动随机生成。
> 除 `/?token=` 首次换取 Cookie 外，所有页面、MJPEG 流与 `/api/*` 接口都需要有效令牌，
> 未授权请求返回 `401`。
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

主要消息类型：`Register 0x03`、`ConnectReq 0x05`、`ConnectRsp 0x06`、`Heartbeat 0x0A`、`AuthChallenge 0x13`、`AuthResponse 0x14`、`AuthResult 0x15`、`SessionClose 0x16`、`CapsNegotiate 0x18`、`MouseMove 0x20`、`MouseButton 0x21`、`MouseWheel 0x22`、`Key 0x23`、`VideoConfig 0x30`、`VideoFrame 0x31`、`KeyframeReq 0x32`、`VideoStats 0x33`、`ClipboardSet 0x50`、`ClipboardData 0x51`、`FileOffer 0x52`、`FileAccept 0x53`、`FileChunk 0x54`、`FileEnd 0x55`、`FileAbort 0x56`。

视频载荷头 32 字节：`capture_ts_us@0`、`encode_ts_us@8`、`frame_id@16`、`width u16@20`、`height u16@22`、`codec u8@24`（1=H.264）、`keyframe@25`、`data_len u32@28`，随后为 Annex-B H.264 数据。

## 核心特性

- **复用单通道**：视频、输入、剪贴板与文件默认走同一 TCP 会话，中继按 `session_id` 转发。
- **能力协商**：`CapsNegotiate` 收敛编码器、帧率与质量；`VideoConfig` 下发参数；`KeyframeReq` 强制 IDR；`VideoStats` 每秒回传编码统计。
- **HMAC-SHA256 鉴权**：中继下发挑战，被控端用密码生成 proof，主控端回传验证。
- **设备身份与防顶号**：被控端用本地 EC P-256 私钥响应中继挑战，中继按设备号固定公钥并持久化，异键注册被拒。
- **可选 TLS**：中继与两端之间支持 mbed TLS 加密，可用 CA、证书指纹或显式不安全模式校验。
- **跨平台采集**：Windows DXGI；Linux X11 或 PipeWire（xdg-desktop-portal）。
- **跨平台注入**：Windows `SendInput`；Linux `/dev/uinput`（支持 Wayland 与 X11）。
- **双向文本剪贴板**：默认关闭，开启后按 500ms 轮询同步，带回环抑制与 256 KiB 上限。
- **文件传输**：主控端可向被控端发送文件（64 KiB 分块、SHA 无关的大小校验），被控端默认关闭接收。
- **Web 网关鉴权**：默认仅监听回环地址，令牌换取 HttpOnly Cookie，页面/MJPEG/API 均需认证。
- **硬件解码**：Android 用 MediaCodec 解码到 Surface，零拷贝上屏。
- **Web 网关节能**：无观看者时降到 2fps，无活动 15 秒后暂停会话；静止帧复用 JPEG 跳过编码。

## 已知限制

- 键盘按 Qt 键码传递，暂不支持输入法组合文本（中文等 IME 候选）。
- Android 客户端暂无文本输入协议，中文输入需依赖被控端已有输入法。
- 多显示器切换、远程分辨率变化后的坐标范围自动刷新尚未实现。
- 音频通道尚未实现（Qt 的音频开关仅做持久化）。
- JPEG 编码路径缺少桌面端解码器，实际互通只使用 H.264。
- 文件传输为单向（主控→被控）、自动接受，无断点续传、进度条与取消按钮。
- Linux 输入注入依赖 `/dev/uinput` 权限，需配置 udev 规则。
- 设备身份只约束被控端；主控端设备号未绑定公钥，且暂无密钥吊销/轮换流程。
- 公钥固定采用 TOFU：设备首次注册前（或身份库丢失后）仍存在抢注窗口。

## 文档

- [远程画面全屏与远程操作更新](res/REMOTE-CONTROL-UPDATE.md)
- [Wayland 屏幕抓取方案调研](res/WAYLAND-CAPTURE-SURVEY.md)
