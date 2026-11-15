# WebRTC 传输 POC

目标：在**不改动现有采集/编码/解码**的前提下，验证把 H.264 流从自定义 TCP 中继换成 WebRTC
（RTP + DTLS/SRTP）后是否可行，并给出可量化的延迟/码率/丢帧基线。

POC 是单进程内两个 PeerConnection 通过回调交换 SDP/ICE 的回环程序，网络走 127.0.0.1 的
host candidate，因此不需要 STUN/TURN，也不依赖现有中继。

## 依赖准备

依赖放在 `third_party/`，不纳入版本库：

| 目录 | 内容 |
| --- | --- |
| `third_party/libdatachannel-0.24.6` | libdatachannel 源码，`deps/` 下补齐 libjuice / libsrtp / plog / usrsctp |
| `third_party/mbedtls-3.6.2` | Mbed TLS 源码（DTLS 后端），`framework/` 需补齐 |
| `third_party/mbedtls-install` | Mbed TLS 安装产物（头文件 + 静态库） |

Mbed TLS 需先单独构建并安装：

```powershell
cmake -S third_party/mbedtls-3.6.2 -B build-mbedtls -G "Visual Studio 17 2022" -A x64 `
      -DUSE_SHARED_MBEDTLS_LIBRARY=OFF -DUSE_STATIC_MBEDTLS_LIBRARY=ON `
      -DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF `
      -DCMAKE_INSTALL_PREFIX=<repo>/third_party/mbedtls-install
cmake --build build-mbedtls --config Release
cmake --install build-mbedtls --config Release
```

## 构建与运行

```powershell
cmake -S . -B build-webrtc -G "Visual Studio 17 2022" -A x64 `
      -DRC_ENABLE_WEBRTC_POC=ON -DENABLE_QT=OFF
cmake --build build-webrtc --config Release --target rc-webrtc-poc
.\build-webrtc\Release\rc-webrtc-poc.exe --capture synthetic --seconds 8
```

参数：`--capture synthetic|dxgi`、`--fps`、`--seconds`、`--quality`。
默认用 `synthetic`，不依赖桌面环境，适合回归。

## 输出与判定

程序打印 ICE/DTLS 状态、轨道与 DataChannel 状态，以及：

- 帧：发出 / 收到（解包后）/ 解码成功，解码成功率应 ≥ 90%；
- 码率：H.264 payload 的 Mbps；
- 端到端延迟：发送（编码完成）→ 解码出图，avg 应 < 300 ms；
- 解码耗时、DataChannel RTT。

全部满足时输出 `PASS` 并以 0 退出，否则 `FAIL` 且返回 1。

## 实测结果（本机回环 127.0.0.1）

| 场景 | 分辨率 | 发出/解码 | 解码成功率 | 码率 | e2e avg / p95 / max | 解码耗时 | DataChannel RTT |
| --- | --- | --- | --- | --- | --- | --- | --- |
| synthetic 30fps ×10s | 1280x720 | 197 / 197 | 100% | 0.40 Mbps | 12.0 / 15.0 / 123.8 ms | 10.9 ms | 0.54 ms |
| synthetic 60fps ×10s | 1280x720 | 202 / 202 | 100% | 0.27 Mbps | 11.1 / 13.6 / 125.2 ms | 10.0 ms | 0.50 ms |
| dxgi 30fps ×8s | 1920x1080 | 144 / 144 | 100% | 1.13 Mbps | 20.2 / 39.0 / 147.5 ms | 18.4 ms | 0.53 ms |

三个场景均 `PASS`：H.264 Annex-B 整帧可以不经改动地经 RTP 打包/解包后被现有解码器还原，
DTLS-SRTP 链路与 SCTP DataChannel 同时工作正常。

读法说明：

- e2e 里约 1 ms 是 RTP 打包 + SRTP + 回环 + 解包，其余是现有 MFT 解码耗时（1080p 软件解码约 18 ms）；
- 码率低是因为合成条纹图和静态桌面都极易压缩，**不代表真实桌面内容**；
- max 是首帧/切换瞬间的离群值，稳态 p95 才是可用指标；
- synthetic 采集本身按 33 ms 节拍出图，所以 `--fps 60` 实际约 20-30fps，属采集端限制。

## 实现要点（迁移时同样适用）

1. **必须自己持有 remote `Track` 的 `shared_ptr`**。libdatachannel 内部只存 `weak_ptr`，
   `onTrack` 回调返回后无人持有时，重新生成 SDP 会把该媒体行标记为 removed（`m=video 0`），
   对端 `openTracks()` 会跳过它，`send()` 永远抛 `Track is not open`。
2. **不能在 `onFrame` 里同步解码**。该回调运行在 libdatachannel 的网络线程上，同步解码会把
   DataChannel RTT 从 0.5 ms 拉到 17 ms 并且阻塞 RTP 收包；解码必须放在独立线程（队列深度 3、
   丢旧帧，和现有主控端一致）。
3. **MF 编码器首帧是异步的**，`encode()` 可能返回失败且错误串为空，要按现有采集循环的方式
   容错（告警后继续），否则一帧都发不出去。
4. 发送不必等 `onOpen`：直接 `send()` 并捕获异常即可，DTLS-SRTP 未就绪时抛异常，下一帧重试。

## 结论

POC 判定 **可行**，可以按阶段 1（libdatachannel 接入 + 复用现有中继做信令）继续推进。
需要提前评估的两项风险：

- 真实弱网下的丢包恢复：本次回环没有丢包，`RtcpNackResponder` / `RtcpReceivingSession` 已接入但未被验证，
  正式迁移前需要真实链路压测；
- 1080p 解码耗时 18 ms 已接近 30fps 预算的 60%，高分辨率场景要确认硬解可用（POC 走的是软件 MFT）。

## 局限

- 回环不经过真实网络，**不代表弱网表现**；丢包/FEC/NACK 行为需要用 `tc netem` 或
  WANem 之类工具在真实链路上再测。
- `--capture dxgi` 目前未把 D3D 设备交给编码器，硬件编码路径需要额外接线。

## 阶段 1：接入现有中继（已实现）

传输层落在 `core/webrtc/webrtc_transport.{h,cpp}`，只替换视频/输入的承载方式，
采集、编码、解码与中继代码不变。

### 开关与构建

```powershell
cmake -S . -B build-webrtc -DRC_ENABLE_WEBRTC=ON -DRC_ENABLE_WEBRTC_POC=ON `
      -DENABLE_QT=ON -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64
cmake --build build-webrtc --config Release --target rc-cli rc-relay rc-app
```

`RC_ENABLE_WEBRTC` 默认 OFF。关闭时 `runner.cpp` 的 WebRTC 分支整体被 `#ifdef` 裁掉，
行为与之前完全一致（旧 TCP 路径为默认值）。

### 信令

新增三个 MsgType，载荷均为 JSON，继续走原有 24 字节帧头与中继会话转发，
**中继无需改动**：

| 消息 | 方向 | 载荷 |
| --- | --- | --- |
| `WebRtcOffer` (0x70) | 被控端 → 主控端 | `{"type":"offer","sdp":"..."}` |
| `WebRtcAnswer` (0x71) | 主控端 → 被控端 | `{"type":"answer","sdp":"..."}` |
| `WebRtcCandidate` (0x72) | 双向 | `{"candidate":"...","mid":"video"}` |

协商顺序：

1. 主控端在 `ConnectReq.want.transport` 里声明 `"webrtc"`（`SessionConfig::transport`）；
2. 被控端鉴权通过后在 `AuthResult.transport` 回传最终方式，请求了但本端未编译
   WebRTC 时回落 `"tcp"`；
3. 被控端创建 PeerConnection（视频轨 SendOnly + `input` DataChannel），
   自动生成的 offer 直接经当前中继连接发出；
4. 主控端收到 offer 后生成 answer，并回传本地 ICE 候选；
5. 视频走 RTP（H.264，PT 96），输入走 DataChannel，格式为
   `[MsgType 1 字节][原 TCP 输入载荷]`；
6. WebRTC 模式下控制连接没有视频帧，被控端每秒发一次 Heartbeat 保活。

### 使用

```powershell
rc-relay --port 21116
rc-cli host --id win-test --password p --capture dxgi --codec h264
rc-cli connect win-test --password p --transport webrtc --render
```

Qt 主控端在「网络」页新增「WebRTC 传输（实验）」开关（仅 `RC_ENABLE_WEBRTC` 构建可见）。

### 实测（本机 + 中继，synthetic 1280x720@30）

- 信令与媒体链路均建立成功，60 帧全部收到、49 帧完成解码；
- 编码/解码模块零改动，旧的 `--transport tcp` 路径回归通过。

## 阶段 2：STUN/TURN 与 NAT 穿透（已实现）

### 配置下发

中继在启动时配置 ICE 服务器，并在 `RegisterAck` 里下发给所有注册端，
被控端与主控端无需各自填写：

```powershell
rc-relay --port 21116 `
         --stun stun.l.google.com:19302 `
         --turn user:pass@turn.example.com:3478
```

- `--stun` / `--turn` 均可重复，`turn:`/`turns:` 与 `stun:` 前缀可省略；
- 下发格式：`{"ok":true,"ice":{"servers":["stun:...","turn:user:pass@..."]}}`；
- 端侧本地配置优先于中继下发，便于临时切换服务器。

### 端侧覆盖

```powershell
# CLI：只对本次连接/被控生效
rc-cli connect win-test --password p --transport webrtc `
       --stun stun.example.com:3478 `
       --turn user:pass@turn.example.com:3478 `
       --ice-relay          # 只使用 TURN 中继候选
       --ice-tcp            # 允许 ICE over TCP 候选
```

Qt 主控端「网络」页新增 ICE 服务器输入框（分号/逗号分隔）与「仅使用 TURN 中继」开关，
配置会随连接保存。

### 可观测性

连接建立时两端都会打印实际选中的候选对：

```
[webrtc] host selected pair local=candidate:... typ host remote=candidate:... typ host
```

候选类型会显示 `host` / `srflx` / `relay`，可直接判断是否真正用上了 STUN 或 TURN。
ICE 服务器地址解析失败时，libjuice 会输出 `Address resolution failed` 告警，
不会中断已有 host 候选的连接。

### 实测

| 场景 | 结果 |
| --- | --- |
| 中继下发 1 个 STUN | 两端均记录 `ice servers=1`，会话建立并解码 49 帧 |
| 中继下发 STUN+TURN，主控本地覆盖为不可达地址 | 本地配置生效（`ice servers=2`），STUN/TURN 解析失败告警后仍经 host 候选连通 |

### 已知限制

- 无 TURN 服务器可验证，`--ice-relay` 的纯中继路径只做了配置接线，未做真实链路压测；
- 未实现 ICE 重启：连接进入 Failed 后需重新建会话，弱网恢复仍待补；
- 未做 TURN 长连接复用与带宽配额控制；
- 发送侧没有码率自适应与关键帧请求，弱网恢复能力待验证；
- DataChannel 输入未做可靠/有序策略调整，仍按 libdatachannel 默认（可靠有序）；
- 当前按会话建立/销毁 PeerConnection，尚未做长连接复用。
