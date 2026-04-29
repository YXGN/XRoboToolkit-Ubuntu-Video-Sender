# XRoboToolkit-Orin-Video-Sender

面向 **Ubuntu（无 CUDA、无 ZED SDK）** 与 **Pico XRoboToolkit** 联调的说明为主；Jetson + 真 ZED 的源码仍保留在仓库中，通过 `Makefile` 注释切换入口。

## 默认构建：Pico `--listen` + USB 摄像头（伪装 ZED 协议）

默认 `make` 生成 `OrinVideoSender`，对应源文件 **`main_zed_webcam_listen.cpp`**：

- 在 `--listen IP:PORT` 上作为 **TCP 服务端**，接收头显发来的 **`OPEN_CAMERA` / `CLOSE_CAMERA`**（载荷格式与 `main_zed_tcp.cpp` 一致）。
- 收到 `OPEN_CAMERA` 后，按载荷中的 **`ip` + `port`** 作为 **TCP 客户端** 连接头显视频接收端，推送 **`4 字节大端长度 + H.264（或 HEVC）`** 码流（与 ZED 版 Sender 一致）。
- 视频来自 **USB 摄像头**：可用 `--camera /dev/videoN` 指定；否则自动选择 **编号升序下第一个能以 1920×1080 采到一帧** 的设备。
- 将单目画面 **左右复制并排**，再缩放到 `OPEN_CAMERA` 中的宽高（BGRA），经 **GStreamer `x264enc` / `x265enc`** 软件编码。

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

**供 Pico 联调（示例：本机监听 13579，可选本机预览与指定摄像头）：**

```bash
./OrinVideoSender --listen 0.0.0.0:13579
# 或绑定到局域网 IP
./OrinVideoSender --listen 192.168.100.24:13579 --preview --camera /dev/video0
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
