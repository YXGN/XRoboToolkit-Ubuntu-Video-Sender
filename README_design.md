# UbuntuVideoSender（zed_webcam）设计说明

本文描述默认 Ubuntu USB 摄像头入口：`main_zed_webcam.cpp` 与 `zed_webcam_{common,listen,send}.cpp`、`uvc_camera_source.cpp` 的职责划分、`--listen` / `--send` 的差异、**libuvc 采集（对齐 teleimager pyuvc）**，以及编码推流管线与 TCP 封包格式。

**推荐启动命令（PC2 + Pico）：**

```bash
./OrinVideoSender --listen 0.0.0.0:13579 --stereo --uvc-serial 01.00.00
```

---


## 一、源码模块

| 文件 | 职责 |
|------|------|
| `main_zed_webcam.cpp` | 唯一 `main`：`gst_init`、参数解析（`--uvc-uid` / `--uvc-serial` / `--stereo`）、安装 SIGINT、调用 `run_listen_mode` 或 `run_send_mode`。 |
| `uvc_camera_source.hpp` / `.cpp` | **libuvc** 打开设备、协商 **MJPEG** 流、回调缓存 JPEG、`readBgr()` 用 OpenCV 解码；`listDevices()` / `reloadUvcDriver()`。 |
| `zed_webcam_common.hpp` / `.cpp` | 全局状态、`CameraRequestData`、`initialize_sender`、`startStreamingThread` / `stopStreamingThread`、`streamingThreadFunction`、GStreamer 管线、`appsink` 回调拼 TCP 包；推流循环调用 **`UvcCameraSource`**。 |
| `zed_webcam_listen.hpp` / `.cpp` | Pico 控制协议：`TCPServer`、`OPEN_CAMERA` / `CLOSE_CAMERA` 处理、`run_listen_mode`。 |
| `zed_webcam_send.hpp` / `.cpp` | 直连模式：CLI 填充配置后 `run_send_mode` → `startStreamingThread`。 |

---

## 二、`--listen` 与 `--send` 的区别

两种模式在 **`main_zed_webcam.cpp` 中互斥**（必须且仅能选其一），**共用** SIGINT 处理、`--preview`、`--uvc-uid` / `--uvc-serial` / `--stereo`（经 `zed_webcam_set_uvc_options`）。

### `--listen`（与 Pico 联调）

- 在本机 **`--listen IP:PORT`** 上启动 **`TCPServer`**，作为 **控制面**：接收 Pico 发来的二进制命令。
- **收到 `OPEN_CAMERA`**：反序列化载荷 → 写入 **`current_camera_config`**，并设置 **`send_to_server` / `send_to_port`**（头显指定的视频回连地址）→ **`startStreamingThread()`**。
- **`CLOSE_CAMERA`** 或 **客户端断开**：**`stopStreamingThread()`**。
- **分辨率 / fps / 码率 / HEVC** 均由 **`OPEN_CAMERA`** 内的 **`CameraRequestData`** 决定。

### `--send`（无 Pico 控制通道）

- **无** `TCPServer`，不解析 Pico 协议。
- 启动时用 CLI **`--server` / `--port`** 以及 **`--width`、`--height`、`--fps`、`--bitrate`、`--hevc`**（另有默认值）填充 **`current_camera_config`** 与 **`send_to_server` / `send_to_port`**，随即 **`startStreamingThread()`**。

### 小结

| 维度 | `--listen` | `--send` |
|------|------------|----------|
| 控制通道 | `TCPServer` + `OPEN_CAMERA` / `CLOSE_CAMERA` | 无 |
| 何时开流 | 收到 `OPEN_CAMERA` 后 | 进程启动后立刻 |
| 视频 TCP 目标 | 载荷中的 `ip` + `port` | `--server` + `--port` |
| 编码参数来源 | Pico 下发的 `CameraRequestData` | CLI |

**采集 → 编码 → TCP 封包发送** 的逻辑两条路径完全一致，均在 **`zed_webcam_common.cpp` 的 `streamingThreadFunction()`** 中实现。

---

## 三、心智图（数据与控制流）

```text
--listen:
  Pico ──TCP(控制)──► TCPServer ──OPEN_CAMERA──► 填写 current_camera_config + 视频 TCP 目标
                                                      │
                                                      ▼
                                            startStreamingThread()

--send:
  CLI 参数 ───────────────────────────────► 填写 current_camera_config + 视频 TCP 目标
                                                      │
                                                      ▼
                                            startStreamingThread()

共用（zed_webcam_common + uvc_camera_source）:
  TCPClient(视频) ◄── initialize_sender()
  libuvc MJPEG ──► imdecode(BGR) ──► BGRA ──► (mono: hconcat / stereo: 直通)
       ──► resize(config.width × config.height) ──► appsrc
  appsrc ──► x264enc 或 x265enc ──► appsink ──► [4B 大端长度][payload] ──► TCP
```

---

## 四、UVC 采集层（对齐 teleimager / pyuvc）

### 4.1 背景

- **PC2（Unitree G1）** 上常见 **无 `/dev/video*`**，OpenCV **V4L2** 路径不可用。
- **xr_teleoperate teleimager** 在 PC2 使用 Python **`uvc.Capture(uid)`** + **`MJPG`**，不经过 V4L2 节点。
- 本 Sender 用 **libuvc** 实现同等能力，设备枚举仍建议用 **同一套 Python API** 查 uid/serial。

### 4.2 设备选择

`UvcCameraSource::open(uid, serial, stereo_sbs, fps)`：

1. 可选 **`reloadUvcDriver()`**（`modprobe -r/+ uvcvideo`），便于从异常状态恢复；可能与 **teleimager** 争用，联调时通常应停 teleimager。
2. **`uvc_get_device_list`**，按 **`--uvc-uid`**（`bus:addr`）和/或 **`--uvc-serial`** 匹配；都为空则选第一台。
3. **`uvc_open`** → **`findStreamCtrl`** → **`uvc_start_streaming`**，回调 **`frameCallback`** 更新 **`latest_jpeg_`**。

### 4.3 MJPEG 模式协商（`findStreamCtrl`）

| 模式 | 尝试顺序（宽×高） | fps |
|------|-------------------|-----|
| **`--stereo`** | **2560×720** → 3840×1080 | 优先 `OPEN_CAMERA` / CLI 的 fps，失败再试 30、60、25… |
| **单目** | 1920×1080 → 2560×720 → 1280×720 → … | 同上 |

与 Python 侧等价关系示例：

```python
cap = uvc.Capture('1:4')
cap.frame_mode = (2560, 720, 60, 'MJPG')
```

对应 C++：`tryMjpegMode(2560, 720, 60, ctrl)`。

### 4.4 读帧与线程模型

- **生产者**：libuvc 内部线程 → **`frameCallback`**（持锁写入 JPEG 缓冲）。
- **消费者**：**`streamingThreadFunction`** 主循环 → **`readBgr()`**（拷贝 JPEG 后 **`cv::imdecode`**）。
- 解码失败（如 USB 带宽不足导致 **截断 MJPEG**）时循环打印 `read frame failed` 并退出；双目在 USB2 上应优先 **2560×720@60**，避免默认 **3840×1080@60**。

### 4.5 已移除的 V4L2 路径

- 删除 **`openUsbCapture`**、**`pickAuto1080pDevice`** 及 CLI **`--camera` / `--stereo-camera`**。
- 不再读取 **`/dev/video*`**。

---

## 五、编码与推流管线（实现要点）

1. **`initialize_sender()`**  
   使用 **`send_to_server` / `send_to_port`** 创建 **`TCPClient`** 并连接（失败重试）。

2. **UVC → BGR → BGRA**  
   - **`--stereo`**：设备已 SBS，**不** `hconcat`；**`resize`** 到 **`config.width × config.height`**（通常与 Pico **2560×720** 一致）。  
   - **单目**：**`hconcat`** 左右复制后 **`resize`**。

3. **GStreamer 管线（`buildWebcamPipelineString`）**  
   - **`appsrc`**：`BGRA`，宽高 fps 来自 **`CameraRequestData`**。  
   - **`videoconvert ! tee`**：一路编码；若 **`preview`**，另一路 **`autovideosink`**。  
   - **H.264**：**`I420` → `x264enc`（profile=high 等）→ `h264parse` → Annex B byte-stream + AU 对齐 → `appsink`**。  
   - **HEVC**：**`enableMvHevc` 非零** 时 **`x265enc → h265parse → appsink`**。  
   - **`appsink`**：`sync=false`，尽快取出编码帧。

4. **TCP 输出（`on_new_sample`）**  
   每个 **`appsink`** 缓冲：**前 4 字节为大端无符号载荷长度**，后跟 **整段编码负载**，与 Pico / ZED Sender **长度前缀 + 码流** 约定一致。

---

## 六、延伸阅读

- 运行参数、PC2 排障、Python 枚举示例：仓库根目录 **`README.md`**。  
- 接收端需兼容同一 TCP 帧格式的播放器：参见 **`README.md`** 中的 VideoPlayer / Video-Viewer 链接。
