# XRoboToolkit-Ubuntu-Video-Sender

面向 **Ubuntu（无 CUDA、无 ZED SDK）** 与 **Pico XRoboToolkit** 联调的说明为主；Jetson + 真 ZED 的源码仍保留在仓库中，通过 `Makefile` 注释切换入口。

## 默认构建：USB 摄像头 `--listen`（Pico）或 `--send`（直连 TCP）

默认 `make` 生成 `OrinVideoSender`，入口为 **`main_zed_webcam.cpp`**，实现拆在 **`zed_webcam_common.cpp`**（采集与编码推流）、**`zed_webcam_listen.cpp`**（控制协议）、**`zed_webcam_send.cpp`**（直连模式）。

### `--listen`（与 Pico 联调）

- 在 `--listen IP:PORT` 上作为 **TCP 服务端**，接收头显发来的 **`OPEN_CAMERA` / `CLOSE_CAMERA`**。载荷格式见下文「传输协议概要」。
- 收到 `OPEN_CAMERA` 后，按载荷中的 **`ip` + `port`** 作为 **TCP 客户端** 连接头显视频接收端，推送 **`[4 字节大端长度][XRLT 头][H.264 / HEVC payload]`** 码流。
- 视频来自 **USB 摄像头**：可用 `--camera /dev/videoN` 指定；否则自动选择 **编号升序下第一个能以 1920×1080 采到一帧** 的设备；双目 SBS 用 **`--stereo-camera`**。
- 单目画面 **左右复制并排** 后再缩放到 `OPEN_CAMERA` 中的宽高（BGRA），**双目 SBS 设备**则直通不复制，经 **GStreamer `x264enc` / `x265enc`** 软件编码。
- H.264 路径默认：**I420 + profile High**（`appsrc → videoconvert → I420 → x264enc → h264parse → appsink`），输出 **Annex B（byte-stream）** + AU 对齐，无需额外参数即可供 Pico 解码。

### `--send`（无 Pico 控制通道）

- 使用 **`--send --server IP --port PORT`**（可选 `--protocol udp`），按命令行分辨率/帧率/码率（可选 `--hevc`）连接接收端并推送 **相同 `[4B 大端长度][XRLT 头][payload]`** 格式。
- 默认值与常见 Pico 请求接近：`2560x720`、`30fps`、`20000000` bps；可用 **`--width` / `--height` / `--fps` / `--bitrate`** 覆盖。
- **`--camera` / `--stereo-camera` / `--preview` / `--zmq` / `--zmq-raw`** 与 `--listen` 共用。

### `--listen + ZMQ` 数采约定（与 `xr_teleoperate` 并存）

当同时配置 **`--listen IP:PORT`** 与 **`--zmq` / `--zmq-raw`** 时，行为由 `zed_webcam_listen.cpp` / `zed_webcam_common.cpp` 严格定义：

1. Sender **先自动启动本地采集与 ZMQ 发布**，不再依赖 Pico 先发 `OPEN_CAMERA`。
2. **`CLOSE_CAMERA` / Pico 断开**只停止 **TCP 发往 Pico** 的那一路；**不会停掉本地采集与 ZMQ 数采**（见 `zed_webcam_common.cpp:701 stopTcpSending()`）。
3. **raw 数采分辨率**由相机输入决定，**不再跟随 `OPEN_CAMERA` 中的 width/height**（见 `agents.md` §3）。例如输入 `1856x800` SBS 时，`--zmq-raw` 只发 LEFT 半幅 `928x800`。
4. `OPEN_CAMERA` 到达时只触发 **encoded pipeline 重配**，**不重启采集线程**。

这是为了避免「Pico 接入 → 采集线程被踢 → host 侧 `episode_writer.py` 报对齐 warning」的回归。

### 传输协议概要（详细见 `README_video_pipeline.md`）

**TCP 视频（`--send` 或 `--listen` 的 encoded 路径）**

```
[4 字节大端 uint32: body_len][body]
body = [XRLT 头 40 字节][编码 payload]
```

`XRLT` 头（小端，定义在 `zed_webcam_common.cpp:151`）：

| 偏移 | 字段 | 说明 |
|------|------|------|
| 0 | `magic` | `XRLT`（4 字节） |
| 4 | `version` | `2`（u16 LE） |
| 6 | `header_size` | `40`（u16 LE） |
| 8 | `frame_id` | 单调递增（u64 LE） |
| 16 | `sender_capture_utc_us` | 采集时刻（u64 LE） |
| 24 | `sender_send_utc_us` | TCP 发送时刻（u64 LE） |
| 32 | `payload_size` | 紧随其后的编码负载字节数（u32 LE） |
| 36 | `reserved` | `0`（u32 LE） |

**UDP 视频（`--send --protocol udp`）**

大帧会被切成 1463 字节一片，每片格式：
```
[0xFF][is_last 0xFF/0x00][4B total_size BE][4B offset BE][payload]
```
小帧可单包发送，格式与 TCP 相同：`[4B BE len][XRLT][payload]`。

**ZMQ raw（`--zmq-raw`，对接 `xr_teleoperate/teleop/utils/episode_writer.py: ZMQRawCameraReceiver`）**

```
[XRAW][4B version=1 BE][4B width BE][4B height BE][4B channels BE]
[8B frame_seq BE][8B source_wall_time_ns BE][8B source_monotonic_ns BE]
[raw BGRA 像素]
```
字段定义见 `zed_webcam_common.cpp:BuildRawImagePacket`。

**ZMQ encoded（`--zmq`）**

与 TCP 一致：`[4B BE len][XRLT 头][H.264 / HEVC payload]`。

**控制协议（`--listen` 的 `OPEN_CAMERA` / `CLOSE_CAMERA`）**

外层：`[4B big-endian bodyLength][body]`
内层 `NetworkDataProtocol`：`[4B LE cmdLen][cmd][4B LE dataLen][data]`
`OPEN_CAMERA` 载荷 `CameraRequestData`：

| 偏移 | 字段 | 说明 |
|------|------|------|
| 0 | `magic` | `0xCA 0xFE` |
| 2 | `version` | `1` |
| 3..31 | `width/height/fps/bitrate/enableMvHevc/renderMode/port` | i32 LE |
| 32 | `camera_len` | u8，后面跟 UTF-8 字符串 |
| 33+ | `camera` | 头显声明的相机类型，原样回显 |
| | `ip` | 视频回连目标 IP（同样紧凑串） |

`CLOSE_CAMERA` 载荷为空。

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

**Pico 联调 + 数采（`--listen` + `--zmq-raw`，详见下一节）：**

> 下面示例中的 IP 都是**示例**，请换成你机器上的真实地址（`--listen` 后是 Sender 自己绑定 IP；`--server` 后是 Windows PC IP；`--zmq-raw` 后是 Sender 自己监听 IP）。

```bash
./OrinVideoSender --listen 0.0.0.0:13579
# 或绑定到局域网 IP，并打开本机预览、指定单目设备（192.168.100.24 改成本机局域网 IP）
./OrinVideoSender --listen 192.168.100.24:13579 --preview --camera /dev/video0
# Pico 双目显示（SBS 直通）+ 主机数采（ZMQ raw）
./OrinVideoSender --listen 0.0.0.0:13579 --zmq-raw tcp://*:5556 --stereo-camera /dev/video0
```

**直连推流（`--send`，接收端先监听 TCP 或 UDP）：**

```bash
# 192.168.100.59 改成运行 Viewer 的 Windows PC 的真实局域网 IP；端口与 appsettings.json 的 listenPort 一致
./OrinVideoSender --send --server 192.168.100.59 --port 12345 --protocol tcp
# 可选：--width 2560 --height 720 --fps 30 --bitrate 20000000 --hevc
# 加双路 SBS 直通：
#           --stereo-camera /dev/video12
```

**完整 CLI 参数（来自 `main_zed_webcam.cpp`）：**

| 参数 | 说明 | 默认 |
|------|------|------|
| `--listen IP:PORT` | Pico 控制通道：在此地址起 `TCPServer`，等 `OPEN_CAMERA`（与 `--send` 互斥） | — |
| `--send` | 无控制通道直连推流（与 `--listen` 互斥） | — |
| `--server IP` | send 模式接收端 IP | — |
| `--port PORT` | send 模式接收端端口 | — |
| `--protocol tcp\|udp` | send 模式传输协议 | `tcp` |
| `--width W` | 输出宽 | `2560` |
| `--height H` | 输出高 | `720` |
| `--fps N` | 帧率 | `30` |
| `--bitrate BPS` | 码率（bps；`x264enc` 内部换算为 kbps） | `20000000` |
| `--hevc` | 使用 `x265enc` + `h265parse`（默认 H.264 + `x264enc`） | H.264 |
| `--preview` | 本机 `autovideosink` 预览支路 | 关 |
| `--camera /dev/videoN` | 单目设备（mono-copy 后编码） | 自动探测 |
| `--stereo-camera /dev/videoN` | 双目 SBS 设备（直通，按 SBS 编码） | — |
| `--zmq tcp://*:PORT` | ZMQ 发布 encoded 帧（XRLT 封包） | — |
| `--zmq-raw tcp://*:PORT` | ZMQ 发布原始 BGRA 帧（XRAW 封包，对接 `xr_teleoperate`） | — |
| `--help` | 打印 usage | — |

**多机位数采**：仓库根目录提供 `start_four_cams_new.sh`（head + 双 wrist + Pico stereo 共 4 个 sender 实例），通过环境变量指定设备路径和端口：

> 下面 `BIND_IP` 是**示例**（192.168.123.164），请换成实际 Sender 机器的局域网 IP；其余 `*_CAM` 也请按本机 `/dev/videoN` 实际编号替换。

```bash
BIND_IP=192.168.123.164 \
  HEAD_CAM=/dev/video0 LEFT_WRIST_CAM=/dev/video4 RIGHT_WRIST_CAM=/dev/video2 \
  PICO_STEREO_CAM=/dev/video0 \
  ./start_four_cams_new.sh
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
