# UbuntuVideoSender（zed_webcam）设计说明

本文描述默认 Ubuntu USB 摄像头入口：`main_zed_webcam.cpp` 与 `zed_webcam_{common,listen,send}.cpp` 的职责划分、`--listen` / `--send` 的差异，以及编码推流管线与 TCP 封包格式。

---

## 一、源码模块

| 文件 | 职责 |
|------|------|
| `main_zed_webcam.cpp` | 唯一 `main`：`gst_init`、参数解析、安装 SIGINT、调用 `run_listen_mode` 或 `run_send_mode`。 |
| `zed_webcam_common.hpp` / `.cpp` | 全局状态、`CameraRequestData`、`initialize_sender`、`startStreamingThread` / `stopStreamingThread`、`streamingThreadFunction`、GStreamer 管线字符串、`appsink` 回调拼 TCP 包、OpenCV 采集与缩放。 |
| `zed_webcam_listen.hpp` / `.cpp` | Pico 控制协议：`TCPServer`、`OPEN_CAMERA` / `CLOSE_CAMERA` 处理、`run_listen_mode`。 |
| `zed_webcam_send.hpp` / `.cpp` | 直连模式：CLI 填充配置后 `run_send_mode` → `startStreamingThread`。 |

---

## 二、`--listen` 与 `--send` 的区别

两种模式在 **`main_zed_webcam.cpp` 中互斥**（必须且仅能选其一），**共用** SIGINT 处理、`--preview`、`--camera` / `--stereo-camera`（经 `zed_webcam_set_camera_paths`）。

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

共用（zed_webcam_common）:
  TCPClient(视频) ◄── initialize_sender()
  OpenCV 采集 + resize + (可选 mono→左右复制 / stereo SBS 直通)
  appsrc ──► x264enc 或 x265enc ──► appsink ──► [4B 大端长度][payload] ──► TCP
```

---

## 四、编码与推流管线（实现要点）

1. **`initialize_sender()`**  
   使用 **`send_to_server` / `send_to_port`** 创建 **`TCPClient`** 并连接（失败重试）。

2. **OpenCV 采集**  
   - **`--stereo-camera`** 非空：SBS 模式（如 MJPEG 1856×800）；否则单目 **1920×1080** 或 CLI 指定设备 / 自动探测。  
   - 单目： **`hconcat` 复制成双路**；再 **`resize`** 到 **`config.width × config.height`**。

3. **GStreamer 管线（`buildWebcamPipelineString`）**  
   - **`appsrc`**：`BGRA`，宽高 fps 来自 **`CameraRequestData`**。  
   - **`videoconvert ! tee`**：一路编码；若 **`preview`**，另一路 **`autovideosink`**。  
   - **H.264**：**`I420` → `x264enc`（profile=high 等）→ `h264parse` → Annex B byte-stream + AU 对齐 → `appsink`**。  
   - **HEVC**：**`enableMvHevc` 非零** 时 **`x265enc → h265parse → appsink`**。  
   - **`appsink`**：`sync=false`，尽快取出编码帧。

4. **TCP 输出（`on_new_sample`）**  
   每个 **`appsink`** 缓冲：**前 4 字节为大端无符号载荷长度**，后跟 **整段编码负载**，与 Pico / ZED Sender **长度前缀 + 码流** 约定一致。

---

## 五、延伸阅读

- 运行参数与用户示例：仓库根目录 **`README.md`**。  
- 接收端需兼容同一 TCP 帧格式的播放器：参见 **`README.md`** 中的 VideoPlayer / Video-Viewer 链接。
