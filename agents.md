# agents.md

本文件给后续维护者 / 代码代理一个**最小可用的项目导航图**，重点说明：

1. 默认构建链路在哪里；
2. webcam / zed / sender / receiver 协议分别落在哪些文件；
3. 修改时哪些地方必须一起改。

---

## 1. 当前默认入口

`Makefile` 默认编译：

- `main_zed_webcam.cpp`
- `zed_webcam_common.cpp`
- `zed_webcam_listen.cpp`
- `zed_webcam_send.cpp`
- `webcam_capture_source.cpp`

也就是说，**当前默认产物是 Ubuntu + USB Webcam 版本**，不是历史的 Jetson + ZED SDK 版本。

---

## 2. 模块职责地图

### A. CLI / 入口层

#### `main_zed_webcam.cpp`
- 解析 `--listen` / `--send`
- 解析 `--preview` / `--camera` / `--stereo-camera`
- 安装 SIGINT 处理
- 分发到：
  - `run_listen_mode(...)`
  - `run_send_mode(...)`

#### `zed_webcam_send.cpp`
- 直连推流模式
- 用 CLI 参数填充 `current_camera_config`
- 设置 `send_to_server` / `send_to_port`
- 调 `startStreamingThread()`

#### `zed_webcam_listen.cpp`
- Pico / Unity 控制通道
- 接收 `OPEN_CAMERA` / `CLOSE_CAMERA`
- 反序列化 `CameraRequestData`
- 更新 `current_camera_config`
- 调 `startStreamingThread()` / `stopStreamingThread()`

---

### B. 采集 / 编码 / 发送主链路

#### `zed_webcam_common.cpp`
这是当前默认版本的**核心文件**：

- 全局状态：
  - `stop_requested`
  - `streaming_active`
  - `encoding_enabled`
  - `send_enabled`
  - `current_camera_config`
- 视频发送端初始化：`initialize_sender()`
- 推流线程生命周期：
  - `startStreamingThread()`
  - `stopStreamingThread()`
  - `streamingThreadFunction()`
- GStreamer pipeline 构造：`buildWebcamPipelineString(...)`
- `appsink` 回调：`on_new_sample(...)`
- 当前 webcam 版视频封包：`BuildTransportPacket(...)`

#### `webcam_capture_source.cpp/.hpp`
负责**摄像头采集侧低延迟化**：

- 后台线程持续 `cap.read()`
- 只保留最新帧（覆盖旧帧）
- 主推流线程通过 `waitNewFrame()` 等待新帧
- 避免主线程重复编码旧帧

这是 webcam 版延迟优化的关键模块。

---

### C. 网络层

#### `network_helper.hpp`
- `TCPClient`：视频发送
- `TCPServer`：控制连接监听

注意：
- `TCPServer` 目前是**按 recv 分片直接回调**，不是严格的流式拆包器；
- 如果控制协议增强，优先先修这里。

---

### D. 历史 / 备用入口

#### `main_zed_tcp.cpp`
历史 Jetson + ZED SDK 主路径：

- 依赖 `sl::Camera`
- 使用 `nvv4l2h264enc / nvv4l2h265enc`
- 视频封包仍是旧格式：`[4B len][payload]`

#### `main_web_gst.cpp`
更早期的最小 webcam + GStreamer + TCP 示例。

#### `main_zed_asio*.cpp` / `main_zed_tcp_zmq.cpp`
历史实验分支，分别测试 asio / UDP / ZMQ 等方案。

如果你的目标是“修当前正式链路”，优先不要从这些入口开始改。

---

## 3. 当前协议现状

### 控制协议

`--listen` 模式下：

- Sender 先作为 TCP Server 等待控制连接
- 控制命令：
  - `OPEN_CAMERA`
  - `CLOSE_CAMERA`
- `OPEN_CAMERA` 里包含：
  - width / height / fps / bitrate
  - HEVC 开关
  - 回连目标 `ip` / `port`

### 视频协议

当前 **webcam 默认链路** 已不是老的纯：

- `[4B big-endian len][H264/H265 payload]`

而是：

- `[4B big-endian len][XRLT 40B header][encoded payload]`

其中 `XRLT` 头里带：

- `frame_id`
- `sender_capture_utc_us`
- `sender_send_utc_us`
- `payload_size`

而**历史 ZED 路径** `main_zed_tcp.cpp` 仍然使用旧格式。

这意味着：

- 默认 webcam sender
- 历史 zed sender
- 外部 receiver / viewer

三者的协议**不再天然一致**。

---

## 4. 改动联动规则

### 改控制协议时
必须同时检查：

- `zed_webcam_listen.cpp`
- `main_zed_tcp.cpp`
- `XRoboToolkit-Unity-Client` 中的命令发送端

### 改视频封包格式时
必须同时检查：

- `zed_webcam_common.cpp`
- `main_zed_tcp.cpp`
- 外部接收端（例如 Windows viewer / Unity viewer / Native viewer）
- `README.md` / `README_design.md` / `README_video_pipeline.md`

### 改摄像头采集策略时
必须同时检查：

- `webcam_capture_source.cpp`
- `zed_webcam_common.cpp`
- `latency_tracker.hpp`

### 改线程停启逻辑时
必须同时检查：

- `startStreamingThread()`
- `stopStreamingThread()`
- `on_new_sample(...)`
- `onDisconnectCallback()`

因为这里存在回调线程与停止线程并发。

---

## 5. 当前维护优先级建议

如果要继续迭代当前项目，建议优先级如下：

1. **先统一视频协议**
   - webcam 默认链路 vs 历史 zed 链路 vs receiver 文档
2. **再修控制 TCP 拆包**
   - 不要依赖一次 `recv()` 就拿到完整命令
3. **再补热重配置**
   - 新 `OPEN_CAMERA` 到达时是否需要 stop + restart
4. **最后再做性能优化**
   - 编码参数、HEVC、零拷贝、更多 trace

---

## 6. 最短阅读路径

新接手本仓库时，建议按这个顺序读：

1. `Makefile`
2. `main_zed_webcam.cpp`
3. `zed_webcam_common.hpp`
4. `zed_webcam_common.cpp`
5. `webcam_capture_source.cpp`
6. `zed_webcam_listen.cpp`
7. `network_helper.hpp`
8. `main_zed_tcp.cpp`（只用于理解历史 ZED 路径差异）

