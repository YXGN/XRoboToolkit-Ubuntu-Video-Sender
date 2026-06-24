# XRoboToolkit-Ubuntu-Video-Sender

面向 **Ubuntu（无 CUDA、无 ZED SDK）** 与 **Pico XRoboToolkit** 联调的说明为主；Jetson + 真 ZED 的源码仍保留在仓库中，通过 `Makefile` 注释切换入口。

## 默认构建：USB 摄像头 `--listen`（Pico）或 `--send`（直连 TCP）

默认 `make` 生成 `OrinVideoSender`，入口为 **`main_zed_webcam.cpp`**，实现拆在 **`zed_webcam_common.cpp`**（采集与编码推流）、**`zed_webcam_listen.cpp`**（控制协议）、**`zed_webcam_send.cpp`**（直连模式）。

### `--listen`（与 Pico 联调）

- 在 `--listen IP:PORT` 上作为 **TCP 服务端**，接收头显发来的 **`OPEN_CAMERA` / `CLOSE_CAMERA`**（载荷格式与 `main_zed_tcp.cpp` 一致）。
- 收到 `OPEN_CAMERA` 后，按载荷中的 **`ip` + `port`** 作为 **TCP 客户端** 连接头显视频接收端，推送 **`4 字节大端长度 + H.264（或 HEVC）`** 码流（与 ZED 版 Sender 一致）。
- 若同时配置 **`--zmq` / `--zmq-raw`**，则会在 listen 模式下**先自动启动本地采集与 ZMQ 发布**，不再依赖 Pico 先发 `OPEN_CAMERA`；这样可同时支持**Pico 看图**与**主机侧数采**。
- 在上述 listen+ZMQ 并存场景下，**`CLOSE_CAMERA` / Pico 断开**只会停止 **TCP 发往 Pico** 的一路，**不会停掉本地采集与 ZMQ 数采**。
- 视频来自 **USB 摄像头**：可用 `--camera /dev/videoN` 指定；否则自动选择 **编号升序下第一个能以 1920×1080 采到一帧** 的设备；双目 SBS 用 **`--stereo-camera`**。
- 将单目画面 **左右复制并排**（或 SBS 直通），再缩放到 `OPEN_CAMERA` 中的宽高（BGRA），经 **GStreamer `x264enc` / `x265enc`** 软件编码。
- H.264 路径默认：**I420 + profile High**，且 **`h264parse` 后固定 Annex B（byte-stream）**，无需额外参数即可供 Pico 解码。

### `--send`（无 Pico 控制通道）

- 使用 **`--send --server IP --port PORT`**，按命令行分辨率/帧率/码率（可选 **`--hevc`**）连接接收端并推送 **相同长度前缀 + 码流** 格式。
- 默认值与常见 Pico 请求接近：`2560x720`、`30fps`、`4000000` bps；可用 **`--width` / `--height` / `--fps` / `--bitrate`** 覆盖。
- **`--camera` / `--stereo-camera` / `--preview`** 与 `--listen` 共用。

### 依赖（Ubuntu）

```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential pkg-config \
  libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libglib2.0-dev \
  gstreamer1.0-tools gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
  gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly gstreamer1.0-libav \
  libopencv-dev libssl-dev libzmq3-dev \
  libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libavdevice-dev
```

确认软件编码器：

```bash
gst-inspect-1.0 x264enc
gst-inspect-1.0 x265enc   # 仅当 Pico 请求 HEVC 时需要
```

### 编译与运行

```bash
make
./OrinVideoSender --help
```

### OpenCV 采集链路独立测速

如果你怀疑问题不在网络而在 `OpenCV VideoCapture -> cap.read()`，可以先独立测速：

```bash
python3 opencv_fps_probe.py --device /dev/video0 --backend v4l2 --width 640 --height 480 --fps 30 --fourcc MJPG --count 300
```

这个脚本只做：

- `OpenCV` 打开设备
- 请求指定 `FOURCC / 分辨率 / FPS`
- 循环 `cap.read()`
- 打印区间 fps 与总体 fps

如果这里已经只有 `15fps`，问题就在 `OpenCV` 采集路径，不在 sender 的 ZMQ/TCP 传输路径。

如果怀疑 Python `cv2` 与 sender 链接的系统 OpenCV 不是同一套，可以再测一遍 C++ 版 probe：

```bash
make probe_cpp
./opencv_fps_probe_cpp --device /dev/video0 --backend v4l2 --width 640 --height 480 --fps 30 --fourcc MJPG --count 300
./opencv_fps_probe_cpp --device /dev/video0 --backend gstreamer --width 640 --height 480 --fps 30 --fourcc MJPG --count 300
```

这版和 sender 使用同一套系统 OpenCV，更接近 sender 实际运行环境。

**供 Pico 联调（示例：本机监听 13579，可选本机预览与指定摄像头）：**

```bash
./OrinVideoSender --listen 0.0.0.0:13579
# 或绑定到局域网 IP
./OrinVideoSender --listen 192.168.100.24:13579 --preview --camera /dev/video0
# Pico 双目显示 + 主机左目 raw 数采
./OrinVideoSender --listen 0.0.0.0:13579 --zmq-raw tcp://*:5556 --stereo-camera /dev/video0
```

**直连推流示例（接收端先监听 TCP）：**

```bash
./OrinVideoSender --send --server 192.168.100.41 --port 12345 --stereo-camera /dev/video12
# 可选：--width 2560 --height 720 --fps 30 --bitrate 4000000 --hevc
```

头显侧按官方流程：选择 ZED 类视频源、输入 **运行 Sender 的机器 IP**、收听。首次联调请在 Sender 终端查看日志里 **`OPEN_CAMERA` 解析出的 `camera` 字符串**（可能为 `ZED`、`ZEDMINI` 等）；当前实现 **不因类型非 `ZED` 而拒绝开流**，便于兼容；确认后再考虑在代码中加白名单。

### 联调检查清单

1. Sender 已监听：`TCPServer is listening on ...`
2. Pico 连接后收到 `OPEN_CAMERA`，日志打印分辨率、fps、`bitrate(bps)`、回连 `ip:port`、`camera` 原样字符串
3. Sender 打印 `Connected to server` 且开始推流后，头显应出现画面
4. 若黑屏/花屏：对照 Pico 请求的 **宽高、H264/HEVC**；尝试仅 H.264；检查 `x264enc` 的 `bitrate`（已由 bps 换算为 kbps）

---

## 备选入口（Makefile 注释切换）

| 源文件 | 用途 |
|--------|------|
| `main_web_gst.cpp` | USB 直推 TCP（`--send`），无 Pico 控制协议 |
| `main_zed_tcp.cpp` 等 | Jetson + ZED SDK + 硬件编码；需恢复 ZED/CUDA 头与库 |

---

## Features（仓库整体）

- Webcam / ZED 相关多入口（见上表）
- Preview、H.264/H.265（依构建与插件）
- TCP/UDP 等历史协议（见各 `main_zed_*.cpp`）

![Screenshot](Docs/screenshot.png)

## One More Thing

- For software encoding ffmpeg, please refer to [RobotVisionTest](https://github.com/XR-Robotics/RobotVision-PC/tree/main/VideoTransferPC/RobotVisionTest).
- For encoded h264 stream receiver, please refer to [VideoPlayer](https://github.com/XR-Robotics/RobotVision-PC/tree/main/VideoTransferPC/VideoPlayer) [TCP Only].
- For a general video player, please refer to [Video-Viewer](https://github.com/XR-Robotics/XRoboToolkit-Native-Video-Viewer) [TCP/UDP].
- The encoded h264 stream can be also played in [Unity-Client](https://github.com/XR-Robotics/XRoboToolkit-Unity-Client) [TCP Only].
