# agents.md

本文件给后续维护者 / 代码代理一个当前有效的维护说明，重点覆盖：

1. 默认构建入口；
2. `listen + Pico + ZMQ raw` 的真实行为；
3. 哪些链路已经解耦，哪些还不能随便改；
4. 与 `xr_teleoperate` 的对接协议。

---

## 1. 当前默认入口

`Makefile` 默认编译：

- `main_zed_webcam.cpp`
- `zed_webcam_common.cpp`
- `zed_webcam_listen.cpp`
- `zed_webcam_send.cpp`
- `webcam_capture_source.cpp`

默认产物是：

- Ubuntu
- USB webcam / UVC camera
- OpenCV 采集
- GStreamer 软件编码
- TCP 控制 / TCP 图传
- 可选 ZMQ raw / encoded 输出

不是默认 Jetson + ZED SDK 版本。

---

## 2. 当前主模式

### `--listen`

用途：

- 给 Pico / 3D viewer 提供控制口
- 接收 `OPEN_CAMERA` / `CLOSE_CAMERA`
- 按请求建立 TCP 视频回连

当前行为：

- 如果只开 `--listen`，Sender 先等 `OPEN_CAMERA`，再起编码视频输出
- 如果同时开了 `--listen` 和 `--zmq` / `--zmq-raw`
  - 会先自动启动本地采集
  - 先启动 ZMQ 输出
  - 不再依赖 Pico 先发 `OPEN_CAMERA`

这点是为了支持：

- Pico 看图
- host 侧数采

同时进行。

### `--send`

用途：

- 无控制口
- 直接按 CLI 参数开始输出

可只开：

- TCP
- ZMQ
- 或两者同时开

---

## 3. 当前关键设计：raw 与 encoded 已部分解耦

当前必须理解这件事：

- **raw/ZMQ 数采链路**
- **encoded/TCP Pico 显示链路**

现在不再应该被当成同一条输出逻辑。

### raw / ZMQ

特点：

- 直接基于采集到的 BGRA 帧构造 `XRAW`
- 不再跟随 Pico `OPEN_CAMERA` 请求的显示分辨率
- 用于 `xr_teleoperate` 数采

### encoded / TCP

特点：

- 单独有一套 GStreamer encoded pipeline
- 用于 Pico / 3D viewer
- 配置变化时，只重建 encoded pipeline，不重启本地采集线程

### 为什么这样改

这是为了修复之前的两个真实问题：

1. Pico 一接入，raw 数采分辨率被改掉
2. `OPEN_CAMERA` / `CLOSE_CAMERA` 打断数采，导致 host 侧不断报对齐 warning

---

## 4. 当前文件职责

### `main_zed_webcam.cpp`

职责：

- CLI 参数入口
- 解析：
  - `--listen`
  - `--send`
  - `--preview`
  - `--camera`
  - `--stereo-camera`
  - `--zmq`
  - `--zmq-raw`
  - `--width/--height/--fps/--bitrate/--hevc`
- 初始化 `current_camera_config`
- 分发到 `run_listen_mode(...)` 或 `run_send_mode(...)`

注意：

- listen 模式下这里的 `width/height/fps/bitrate`
  现在是 **初始 encoded 配置**
  不是 raw 数采尺寸。

### `zed_webcam_listen.cpp`

职责：

- 控制连接监听
- 解析 `OPEN_CAMERA` / `CLOSE_CAMERA`
- 更新 `current_camera_config`
- 更新 `send_to_server` / `send_to_port`

当前重要行为：

- 当 streaming 已在跑且 ZMQ 已启用时：
  - `OPEN_CAMERA` 不再重启本地采集
  - 只更新 TCP 目标
  - 只触发 encoded pipeline 重配
- `CLOSE_CAMERA` / 断开连接时：
  - 只停 TCP
  - 不停 raw/ZMQ 数采

### `zed_webcam_send.cpp`

职责：

- `--send` 模式填充默认配置
- 设置 TCP 目标
- 启动 `startStreamingThread()`

### `zed_webcam_common.cpp`

这是当前正式主链路核心文件。

负责：

- 全局状态
- ZMQ 初始化 / 清理
- TCP sender 初始化
- raw `XRAW` 封包
- encoded `XRLT` 封包
- streaming thread 生命周期
- encoded pipeline 生命周期
- 采集帧到 raw/encoded 的分发

尤其要看：

- `BuildRawImagePacket(...)`
- `BuildTransportPacket(...)`
- `startStreamingThread()`
- `stopStreamingThread()`
- `stopTcpSending()`
- `streamingThreadFunction()`
- `EncodedPipelineState`

### `webcam_capture_source.cpp/.hpp`

职责：

- 相机后台采集线程
- 始终只保留最新帧
- 避免主线程处理旧帧

这是当前低延迟的基础，不要轻易回退到主线程直接 `cap.read()`。

### `network_helper.hpp`

职责：

- `TCPClient`
- `TCPServer`

注意：

- 控制口仍然没有做严格的 TCP 流式拆包缓冲
- 如果控制包以后变复杂，这里仍然是风险点

---

## 5. 当前协议

### 控制协议

`--listen` 下：

- 外层：`[4B big-endian bodyLength][body]`
- 内层 `NetworkDataProtocol`：
  - `commandLength`：little-endian
  - `dataLength`：little-endian

`OPEN_CAMERA` 载荷 `CameraRequestData`：

- magic: `0xCA 0xFE`
- version: `1`
- 整数字段：little-endian

### encoded 视频协议

当前 webcam 主链路不是旧的纯 `[4B len][payload]`，而是：

- `[4B big-endian len][XRLT header][encoded payload]`

`XRLT` 头里带：

- `frame_id`
- `sender_capture_utc_us`
- `sender_send_utc_us`
- `payload_size`

### raw 图像协议

当前与 `xr_teleoperate` 对接的是：

- `XRAW`
- big-endian

字段：

- `magic = XRAW`
- `version = 1`
- `width`
- `height`
- `channels`
- `frame_seq`
- `source_wall_time_ns`
- `source_monotonic_ns`
- `raw image bytes`

---

## 6. 当前数采语义

### `--camera`

- 单目输入
- raw 数采直接保留原图
- encoded 给 Pico 时会做 mono-copy 生成双路显示

### `--stereo-camera`

- 假设输入是 side-by-side 双目
- encoded 给 Pico 时保留 SBS 语义
- raw 数采时只取 **LEFT**

例如：

- 相机采集 `1856x800`
- raw 数采输出 LEFT half
- 实际数采分辨率为 `928x800`

所以：

- Pico 显示分辨率可为 `2560x720`
- raw 数采分辨率仍可固定是 `928x800`

这是预期行为，不是 bug。

---

## 7. 当前与 xr_teleoperate 的对接结论

对接端是：

- `xr_teleoperate/teleop/utils/episode_writer.py`
  - `ZMQRawCameraReceiver`

重要事实：

- receiver 现在按 `XRAW` 收图
- host 侧对齐依赖的是 `host_recv_monotonic_ns`
- sender 侧 raw 数采如果被断流 / 重启 / 改分辨率，会直接在 host 侧表现为：
  - `waiting for first post-start camera frame...`
  - `skip sample: no aligned camera frame found after record start.`

因此，后续改 sender 时，必须优先保证：

1. raw/ZMQ 数采持续稳定
2. Pico 接入不打断 raw
3. raw shape 不因 viewer 接入而变化

---

## 8. 修改时必须联动检查

### 改控制逻辑时

至少检查：

- `zed_webcam_listen.cpp`
- `network_helper.hpp`
- `XRoboToolkit-Unity-Client`
- Pico / 3D viewer 对端

### 改 raw 协议时

至少检查：

- `zed_webcam_common.cpp`
- `xr_teleoperate/teleop/utils/episode_writer.py`
- 本仓库里的 `zmq_receiver.py`

### 改 encoded 协议时

至少检查：

- `zed_webcam_common.cpp`
- 历史 `main_zed_tcp.cpp`
- viewer / receiver 文档与实现

### 改 streaming thread / stop 行为时

至少检查：

- `startStreamingThread()`
- `stopStreamingThread()`
- `stopTcpSending()`
- `onDisconnectCallback()`
- `handleOpenCamera()`
- `streamingThreadFunction()`

因为这里很容易重新引入“Pico 接入打断数采”的回归。

---

## 9. 当前已知边界

1. 当前已经做到：
   - raw 与 encoded 生命周期部分解耦
   - viewer 接入不会再重启本地采集线程
   - raw 分辨率不应再跟随 `OPEN_CAMERA` 变化

2. 仍需警惕：
   - 高负载下 encoded 与 raw 共进程竞争 CPU
   - 控制 TCP 仍缺稳健拆包
   - 某些 UVC 设备自身可能因驱动协商导致输出抖动

3. 如果后续还出现：
   - raw 帧率明显抖动
   - host 持续对齐 warning

优先方向是：

- 把 raw 发布进一步独立成专用线程 / 队列

而不是再把采集线程和 encoded pipeline 绑回去。

---

## 10. 最短阅读路径

新接手本仓库时，建议按顺序读：

1. `Makefile`
2. `main_zed_webcam.cpp`
3. `zed_webcam_common.hpp`
4. `zed_webcam_common.cpp`
5. `webcam_capture_source.cpp`
6. `zed_webcam_listen.cpp`
7. `network_helper.hpp`
8. `README.md`

如果只是排查当前 Pico + 数采问题，不要先从历史 `main_zed_tcp.cpp` 开始。
