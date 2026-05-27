# PROJECT_DATA_FLOW_AND_DESIGN

本文基于当前仓库代码，对 **XRoboToolkit-Ubuntu-Video-Sender-Webcam-dev** 做一次面向维护者的结构化 review，覆盖：

- sender 主逻辑
- webcam 主逻辑
- 历史 zed 逻辑
- 当前项目的数据流与设计
- 已识别的关键风险点

---

## 1. 项目定位

这个仓库实际上包含了两套思路：

1. **当前默认链路：Ubuntu + USB webcam + GStreamer 软件编码**
   - 入口：`main_zed_webcam.cpp`
   - 默认由 `Makefile` 编译

2. **历史链路：Jetson + ZED SDK + 硬编码器**
   - 入口：`main_zed_tcp.cpp`
   - 需要 ZED SDK / CUDA / Jetson 环境

因此，项目名里虽然还有 `zed`，但**当前默认产品已经是 webcam sender**。

---

## 2. 总体模块图

```text
CLI / Process
  ├─ main_zed_webcam.cpp
  │   ├─ --listen  -> zed_webcam_listen.cpp
  │   └─ --send    -> zed_webcam_send.cpp
  │
  ├─ zed_webcam_common.cpp
  │   ├─ streamingThreadFunction
  │   ├─ initialize_sender
  │   ├─ GStreamer pipeline
  │   └─ appsink -> TCP send
  │
  ├─ webcam_capture_source.cpp
  │   └─ 后台线程持续采集最新帧
  │
  └─ network_helper.hpp
      ├─ TCPClient
      └─ TCPServer
```

历史链路单独在：

```text
main_zed_tcp.cpp
  ├─ ZED SDK grab/retrieveImage
  ├─ GStreamer NVENC
  └─ TCP send
```

---

## 3. 默认 webcam 链路的数据流

## 3.1 `--listen` 模式

```text
Pico / Unity Client
  └─ TCP 控制连接
      └─ OPEN_CAMERA / CLOSE_CAMERA
          └─ zed_webcam_listen.cpp
              ├─ 解析 CameraRequestData
              ├─ 更新 current_camera_config
              ├─ 更新 send_to_server / send_to_port
              └─ startStreamingThread()

streamingThreadFunction()
  ├─ initialize_sender() 连接视频接收端
  ├─ WebcamCaptureSource.open()
  ├─ 后台线程 cap.read() 持续保留最新帧
  ├─ 主线程 waitNewFrame() -> readFrame()
  ├─ BGR -> BGRA
  ├─ mono: 左右复制；stereo: SBS 直通
  ├─ resize 到目标宽高
  ├─ appsrc push 到 GStreamer
  ├─ x264/x265 编码
  ├─ appsink 拉出编码结果
  └─ TCPClient.sendData() 发送到 receiver
```

## 3.2 `--send` 模式

```text
CLI 参数
  ├─ --server / --port
  ├─ --width / --height / --fps / --bitrate
  └─ --hevc
      └─ zed_webcam_send.cpp
          ├─ 填充 current_camera_config
          ├─ 设置 send_to_server / send_to_port
          └─ startStreamingThread()

后续采集、编码、发送链路与 --listen 完全共用
```

---

## 4. 默认 webcam 链路的设计要点

## 4.1 采集线程与推流线程分离

`webcam_capture_source.cpp` 的设计是本项目当前最合理的一部分：

- 后台线程高频 `cap.read()`
- 主线程不直接阻塞在 `VideoCapture::read()`
- 共享缓冲只保留“最新帧”
- 旧帧会被覆盖，不会在 OpenCV / V4L2 / 应用层堆积

这套设计的优点：

- 更低延迟
- 主线程处理速度低于相机帧率时，不会线性积压
- 适合实时观看，而不是录像保真

代价：

- 会主动丢帧
- `clone()` 次数较多，CPU / 内存带宽会增加

---

## 4.2 GStreamer 低延迟策略

`zed_webcam_common.cpp` 里用了统一低延迟队列参数：

```text
queue max-size-buffers=1 max-size-time=0 max-size-bytes=0 leaky=downstream
```

含义：

- 只保留 1 帧
- 编码慢时丢旧帧
- 保留最新帧

这是和 `WebcamCaptureSource` 同方向的设计：**牺牲完整性，换低延迟**。

---

## 4.3 单目兼容 ZED 的策略

当前 webcam 版为了复用“ZED/SBS 接收端”的显示假设，采用：

- 单目：`hconcat(bgra, bgra)`
- 双目 SBS：直接使用摄像头输出

这相当于把单目包装成“伪 SBS”。

优点：

- 不改下游 stereo/VR/3D viewer 的 SBS 解码逻辑

缺点：

- 单目情况下左右眼内容完全相同，不是真立体

---

## 4.4 延迟观测设计

`latency_tracker.hpp` 把链路拆成：

- T1→T2：采集线程到主线程
- T2→T3：颜色转换
- T3→T4：resize
- T4→T5：appsrc push
- T5→T6：GStreamer 编码
- T6→T7：TCP send

这是一个很实用的工程化设计，说明当前版本不是只在“能跑”，而是在针对延迟做可观测优化。

---

## 5. 当前 webcam 视频协议

这里是本次 review 里最重要的点之一。

### 5.1 历史协议

历史 sender / README / 多数 receiver 文档描述的是：

```text
[4 bytes big-endian payload_len][encoded payload]
```

### 5.2 当前 webcam 默认协议

`zed_webcam_common.cpp` 已经改成：

```text
[4 bytes big-endian body_len]
[XRLT 40 bytes transport header]
[encoded payload]
```

其中 `XRLT` 头包含：

- magic = `XRLT`
- version
- header_size
- frame_id
- sender_capture_utc_us
- sender_send_utc_us
- payload_size

### 5.3 设计意义

这个改动的目标很明确：

- 给接收端提供 frame_id
- 支持单向网络延迟统计
- 支持 capture/send/present 级联 trace

### 5.4 设计风险

这个协议已经**不再等价**于历史 zed sender，也**不再等价**于 README 中“4 字节长度 + 码流”的描述。

如果 receiver 仍按旧协议直接把 payload 交给 H.264 解码器，那么会把 `XRLT...` 当成码流头，直接解码失败。

---

## 6. 历史 ZED 链路的数据流

`main_zed_tcp.cpp` 是一体化文件：

```text
Pico OPEN_CAMERA
  └─ 解析控制协议
      └─ 更新 current_camera_config
          └─ startStreamingThread()

streamingThreadFunction()
  ├─ 初始化 ZED SDK
  ├─ zed.grab()
  ├─ retrieveImage(SIDE_BY_SIDE)
  ├─ appsrc -> nvvidconv -> nvv4l2h264enc/h265enc
  ├─ appsink
  └─ [4B len][payload] TCP send
```

特点：

- 依赖 Jetson / NVENC / ZED SDK
- 单文件职责过重
- 协议、控制、采集、编码、网络全耦合在一起

相比之下，当前 webcam 版虽然功能更少，但结构比历史 ZED 版更清晰。

---

## 7. Review 结论：当前设计的优点

## 7.1 正向评价

### 优点 1：webcam 版已经做了模块拆分

相较 `main_zed_tcp.cpp`，当前默认版本把：

- 入口
- listen/send 模式
- 公共推流逻辑
- 摄像头采集

拆成了不同文件，维护成本明显更低。

### 优点 2：低延迟思想一致

从 `WebcamCaptureSource` 到 GStreamer `leaky queue`，再到 `LatencyTracker`，整体设计目标很统一：

- 最新帧优先
- 低延迟优先
- 可观测优先

### 优点 3：listen / send 两种模式共用一条视频主链路

这避免了“控制模式”和“直连模式”各维护一套采集编码逻辑。

---

## 8. Review 结论：关键问题与风险

以下按优先级排序。

## P0 - 控制 TCP 拆包逻辑不正确

`TCPServer::handleClient()` 里每次 `recv()` 后，直接把本次收到的字节块交给 `data_callback`。

但 TCP 是**字节流**，不是消息边界协议。实际可能出现：

- 一个完整命令被拆成多次 `recv()`
- 多个命令被合并到一次 `recv()`

当前 `zed_webcam_listen.cpp` 的 `onDataCallback()` 只尝试解析“当前这一块数据里的一个包”，因此：

- 半包时会报长度不足
- 粘包时只解析第一个包，后续字节被丢弃

这会让 `OPEN_CAMERA / CLOSE_CAMERA` 在网络抖动或不同平台栈上出现随机失效。

**建议**：把控制协议拆包逻辑前移到 `TCPServer`，实现一个真正的 framed reader。

---

## P0 - 当前 webcam 默认视频协议与历史 receiver / 文档不一致

当前 webcam 版发送的是：

```text
[4B len][XRLT 40B header][payload]
```

而历史链路 / 文档 / 部分 receiver 仍按：

```text
[4B len][payload]
```

这会造成：

- 同仓库不同入口协议不一致
- 跨项目对接容易失败
- README 与代码不一致

**建议**：

1. 明确把协议分成 `legacy_raw` 和 `xrlt_v2`
2. 给 CLI 增加显式选项，或统一所有 sender
3. 更新所有 README / 接收端说明

---

## P1 - `OPEN_CAMERA` 热重配置不会真正生效

当前 `handleOpenCamera()` 会更新 `current_camera_config`，然后调用 `startStreamingThread()`。

但如果线程已在运行，`startStreamingThread()` 会直接返回：

- 不会 stop
- 不会 restart
- streaming thread 内部也不会重新读取配置

所以新的：

- 分辨率
- fps
- bitrate
- HEVC 开关
- 视频目标地址

在“已开流状态下再次 OPEN_CAMERA”时，并不会真正生效。

**建议**：

- 明确约束协议：必须 `CLOSE_CAMERA` 后再 `OPEN_CAMERA`
- 或在收到新配置时做 stop + restart

---

## P1 - `sender_ptr` 存在并发访问风险

`on_new_sample()` 在 GStreamer 回调线程里直接读 `sender_ptr` 并调用 `sendData()`；
`stopStreamingThread()` 会在另一个线程里：

- `disconnect()`
- `sender_ptr = nullptr`

这里只有原子布尔，没有对 `sender_ptr` 本身做互斥保护，因此存在：

- 回调线程读指针
- 停止线程同时 reset 指针

的并发风险。

**建议**：

1. 用 mutex 保护 `sender_ptr`
2. 或改成单独网络发送线程 + 无锁/有锁队列
3. 不要在 `appsink` 回调线程直接做同步网络发送

---

## P1 - `--listen IP:PORT` 的 IP 实际没有生效

`TCPServer` 构造时只解析端口，`start()` 里固定：

- `server_addr.sin_addr.s_addr = INADDR_ANY`

也就是说传入的 `IP` 只是表面参数，实际始终绑定：

- `0.0.0.0:<port>`

这和 CLI / README 语义不一致。

**建议**：

- 要么真正支持绑定指定 IP
- 要么把参数改成只接收 `PORT`

---

## P2 - HEVC 路径的封包和码流约束弱于 H.264

H.264 路径显式做了：

- `I420`
- `h264parse`
- `stream-format=byte-stream`
- `alignment=au`

但 HEVC 路径没有对应的：

- 显式 4:2:0 限制
- 显式 AU 对齐约束

这意味着：

- 与接收端的“每包就是一帧/一 AU”假设不一定始终一致
- 兼容性风险高于 H.264

---

## P2 - SIGINT 处理函数里做了过多非异步安全操作

信号处理函数里直接调用了：

- `std::cout`
- `stopStreamingThread()`
- `condition_variable`
- `unique_ptr` 相关析构/停止逻辑

这在严格意义上不是 async-signal-safe 的写法。

工程上不一定立刻炸，但不是稳妥实现。

**建议**：

- 信号里只置位原子标志
- 由主线程轮询后做真实清理

---

## 9. 建议的后续演进方向

## 9.1 先做协议统一

推荐明确区分：

- 控制协议
- 视频协议

并把视频协议版本化，例如：

- `legacy_h264_len_prefix`
- `xrlt_v2`

---

## 9.2 再做网络层下沉

把：

- TCP 拆包
- TCP 重连
- 发送排队
- 断线状态机

从 `zed_webcam_common.cpp` / `zed_webcam_listen.cpp` 下沉到独立网络模块。

---

## 9.3 再统一 webcam 与 zed 的公共抽象

可以抽象成：

- `IVideoSource`
  - `WebcamVideoSource`
  - `ZedVideoSource`

上层只关心：

- 取一帧 SBS BGRA
- 获取时间戳

这样就能避免：

- 当前 webcam 版一套逻辑
- 历史 zed 版另一套逻辑

长期分叉。

---

## 10. 一句话总结

**当前仓库最值得保留的是 webcam 版的低延迟采集/编码结构；最需要尽快修的是控制 TCP 拆包、视频协议分叉，以及运行中重配置不生效这三个问题。**

