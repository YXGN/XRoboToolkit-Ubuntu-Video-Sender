# XRoboToolkit-Ubuntu-Video-Sender

面向 **Ubuntu（无 CUDA、无 ZED SDK）** 与 **Pico XRoboToolkit** 联调的说明为主；Jetson + 真 ZED 的源码仍保留在仓库中，通过 `Makefile` 注释切换入口。

## 默认构建：USB 摄像头 `--listen`（Pico）或 `--send`（直连 TCP）

默认 `make` 生成 `OrinVideoSender`，入口为 **`main_zed_webcam.cpp`**，实现拆在 **`zed_webcam_common.cpp`**（采集与编码推流）、**`zed_webcam_listen.cpp`**（控制协议）、**`zed_webcam_send.cpp`**（直连模式）。

### `--listen`（与 Pico 联调）

- 在 `--listen IP:PORT` 上作为 **TCP 服务端**，接收头显发来的 **`OPEN_CAMERA` / `CLOSE_CAMERA`**（载荷格式与 `main_zed_tcp.cpp` 一致）。
- 收到 `OPEN_CAMERA` 后，按载荷中的 **`ip` + `port`** 作为 **TCP 客户端** 连接头显视频接收端，推送 **`4 字节大端长度 + H.264（或 HEVC）`** 码流（与 ZED 版 Sender 一致）。
- 视频来自 **USB 摄像头（libuvc + MJPEG）**，对齐 xr_teleoperate teleimager 的 `uvc.Capture(uid)`；不依赖 `/dev/video*`。用 **`--uvc-uid`** / **`--uvc-serial`** 指定设备，未指定则用第一台 UVC；双目 SBS 加 **`--stereo`**（采集 1856×800 MJPEG）。
- 将单目画面 **左右复制并排**（或 SBS 直通），再缩放到 `OPEN_CAMERA` 中的宽高（BGRA），经 **GStreamer `x264enc` / `x265enc`** 软件编码。
- H.264 路径默认：**I420 + profile High**，且 **`h264parse` 后固定 Annex B（byte-stream）**，无需额外参数即可供 Pico 解码。

### `--send`（无 Pico 控制通道）

- 使用 **`--send --server IP --port PORT`**，按命令行分辨率/帧率/码率（可选 **`--hevc`**）连接接收端并推送 **相同长度前缀 + 码流** 格式。
- 默认值与常见 Pico 请求接近：`2560x720`、`30fps`、`4000000` bps；可用 **`--width` / `--height` / `--fps` / `--bitrate`** 覆盖。
- **`--uvc-uid` / `--uvc-serial` / `--stereo` / `--preview`** 与 `--listen` 共用。

### 依赖（Ubuntu）

```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential pkg-config \
  libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libglib2.0-dev \
  gstreamer1.0-tools gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
  gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly gstreamer1.0-libav \
  libopencv-dev libssl-dev libzmq3-dev \
  libuvc-dev libusb-1.0-0-dev \
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

**在 PC2 上发现 UVC 设备 uid（与 teleimager 一致）：**

```bash
python3 -c "import uvc; print(uvc.device_list())"
```

**供 Pico 联调（示例：本机监听 13579，双目 + 序列号）：**

```bash
./OrinVideoSender --listen 0.0.0.0:13579
# 或绑定到局域网 IP
./OrinVideoSender --listen 192.168.123.164:13579 --preview --stereo --uvc-serial 01.00.00
```

**直连推流示例（接收端先监听 TCP）：**

```bash
./OrinVideoSender --send --server 192.168.100.41 --port 12345 --stereo --uvc-uid 1:9
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
