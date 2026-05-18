# XRoboToolkit-Ubuntu-Video-Sender

面向 **Ubuntu（无 CUDA、无 ZED SDK）** 与 **Pico XRoboToolkit** 联调的说明为主；Jetson + 真 ZED 的源码仍保留在仓库中，通过 `Makefile` 注释切换入口。

## 启动命令（PC2 / Pico 联调，推荐）

编译完成后，在 **Unitree G1 PC2** 上（先停止 teleimager 以免占用相机）：

```bash
./OrinVideoSender --listen 0.0.0.0:13579 --stereo --uvc-serial 01.00.00
```

头显 **XRRoboToolkit** → Camera → 选择 ZED 类源 → Listen → 填入 **PC2 的 IP** → Confirm。`01.00.00` 为示例序列号，请用 `python3 -c "import uvc; print(uvc.device_list())"` 核对本机 serial。

## 默认构建：USB 摄像头 `--listen`（Pico）或 `--send`（直连 TCP）

默认 `make` 生成 `OrinVideoSender`，入口为 **`main_zed_webcam.cpp`**，实现拆在 **`zed_webcam_common.cpp`**（采集与编码推流）、**`zed_webcam_listen.cpp`**（控制协议）、**`zed_webcam_send.cpp`**（直连模式）、**`uvc_camera_source.cpp`**（libuvc 采集）。

### 采集方式：libuvc + MJPEG（对齐 teleimager / pyuvc）

在 **Unitree G1 PC2（Jetson）** 等环境下，USB 相机往往 **没有** 可用的 V4L2 节点（无 `/dev/video*`，仅有 Tegra `/dev/media*`），无法用 OpenCV `CAP_V4L2` 打开。本仓库默认路径与 **xr_teleoperate** 中 **teleimager** 的 Python 方案一致：

| teleimager（Python） | 本 Sender（C++） |
|----------------------|------------------|
| `import uvc` / `uvc.device_list()` | 启动时 `UvcCameraSource::listDevices()`；联调用 **Python 列设备** |
| `uvc.Capture(uid)` 或按 serial 选设备 | `--uvc-uid` / `--uvc-serial` → `uvc_open` |
| `frame_mode = (W, H, fps, 'MJPG')` | `uvc_get_stream_ctrl_format_size(..., MJPEG, W, H, fps)` |
| `get_frame_robust()` + `cv2.imdecode` | libuvc 回调收 MJPEG → `cv::imdecode` → BGR |

**不依赖** `/dev/video*`；运行时仅需 **libuvc**、**libusb-1.0** 与 **OpenCV**（解码 MJPEG）。

**双目 SBS（`--stereo`）MJPEG 分辨率优先级**（与 Pico 常见 `OPEN_CAMERA` 2560×720@60 对齐）：

1. **2560×720**（首选，USB2 相机带宽更稳）
2. **3840×1080**（回落；高帧率下可能出现 `Corrupt JPEG`，见下方排障）

**单目**（无 `--stereo`）：优先 **1920×1080**，再回落 2560×720、1280×720 等；画面经 **左右复制** 模拟 ZED SIDE_BY_SIDE 后缩放编码。

**已移除**：`--camera`、`--stereo-camera`（V4L2 设备路径）。

### `--listen`（与 Pico 联调）

- 在 `--listen IP:PORT` 上作为 **TCP 服务端**，接收头显发来的 **`OPEN_CAMERA` / `CLOSE_CAMERA`**（载荷格式与 `main_zed_tcp.cpp` 一致）。
- 收到 `OPEN_CAMERA` 后，按载荷中的 **`ip` + `port`** 作为 **TCP 客户端** 连接头显视频接收端，推送 **`4 字节大端长度 + H.264（或 HEVC）`** 码流（与 ZED 版 Sender 一致）。
- 采集分辨率由 UVC 模式决定；**编码输出**宽高、fps、码率以 **`OPEN_CAMERA`** 载荷为准（常见 **2560×720@60**）。SBS 模式下设备已输出左右并排，仅 **resize** 到目标分辨率，不再 `hconcat` 复制。
- 经 **GStreamer `x264enc` / `x265enc`** 软件编码；H.264 路径默认 **I420 + profile High**，**`h264parse` 后 Annex B（byte-stream）**。

### `--send`（无 Pico 控制通道）

- 使用 **`--send --server IP --port PORT`**，按命令行分辨率/帧率/码率（可选 **`--hevc`**）连接接收端并推送 **相同长度前缀 + 码流** 格式。
- 默认值与常见 Pico 请求接近：`2560x720`、`30fps`、`4000000` bps；可用 **`--width` / `--height` / `--fps` / `--bitrate`** 覆盖。
- **`--uvc-uid` / `--uvc-serial` / `--stereo` / `--preview`** 与 `--listen` 共用。

### 依赖（Ubuntu）

**编译 / 运行（C++ Sender）**

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

**设备枚举（与 teleimager 相同，仅需 Python + pyuvc）**

PC2 上若已能跑 teleimager，通常已具备 `uvc` 模块；否则在 teleimager / xr_teleoperate 环境中安装与 **`import uvc`** 相同的包（常见为 **pyuvc**）。仅用于查看 **uid / serial**，Sender 本体不调用 Python。

```bash
python3 -c "import uvc; print(uvc.device_list())"
```

示例输出（字段因版本略有差异）：

```text
# uid 形如 "1:4"（bus:address），serial 用于 --uvc-serial
```

确认软件编码器：

```bash
gst-inspect-1.0 x264enc
gst-inspect-1.0 x265enc   # 仅当 Pico 请求 HEVC 时需要
```

### 编译与运行

```bash
cd ~/XRoboToolkit-Ubuntu-Video-Sender   # 确保当前目录存在（避免 make: getcwd）
make clean && make
./OrinVideoSender --help
```

**在 PC2 上选定相机（与 teleimager 配置对照）**

```bash
python3 -c "import uvc; print(uvc.device_list())"
# 记下目标相机的 uid（如 1:4）与 serial（如 01.00.00）
```

**供 Pico 联调（推荐：双目 SBS + 序列号）**

```bash
# 先确保 teleimager 未占用相机（见排障）
./OrinVideoSender --listen 0.0.0.0:13579 --stereo --uvc-serial 01.00.00
# 本机预览可加 --preview
./OrinVideoSender --listen 192.168.123.164:13579 --preview --stereo --uvc-serial 01.00.00
```

成功日志示例：`[UVC] MJPEG mode 2560x720@60`、`[UVC] streaming started uid=1:4`。

**直连推流示例（接收端先监听 TCP）**

```bash
./OrinVideoSender --send --server 192.168.100.41 --port 12345 --stereo --uvc-uid 1:4
# 可选：--width 2560 --height 720 --fps 60 --bitrate 4000000 --hevc
```

### CLI 摘要（UVC 相关）

| 参数 | 说明 |
|------|------|
| `--stereo` | 双目 SBS：设备直接输出左右拼接 MJPEG，优先 **2560×720**，再试 3840×1080 |
| `--uvc-uid UID` | 与 `uvc.device_list()` 的 uid 一致，如 `1:4` |
| `--uvc-serial SN` | 设备序列号，如 `01.00.00`（与 teleimager YAML 中 serial 对照） |
| （无 uid/serial） | 打开枚举到的第一台 UVC 设备 |

`--listen` 模式下 **fps / 输出分辨率** 以 Pico **`OPEN_CAMERA`** 为准；UVC 采集 fps 会优先匹配该值（失败则回落 30、60 等）。

头显侧按官方流程：选择 ZED 类视频源、输入 **运行 Sender 的机器 IP**、收听。日志中 **`OPEN_CAMERA` 的 `camera` 字符串**（`ZED` 等）当前 **不用于拒绝开流**。

### 联调检查清单

1. Sender 已监听：`TCPServer is listening on ...`
2. Pico 连接后收到 `OPEN_CAMERA`，日志打印分辨率、fps、`bitrate(bps)`、回连 `ip:port`
3. `[UVC] Devices:` 列表中含目标相机；`[UVC] MJPEG mode ...` 与预期一致（双目建议 **2560×720**）
4. `Connected to server` 后开始推流，头显有画面
5. 若黑屏/花屏：对照 **H264/HEVC**、码率；确认 `x264enc` 可用

### 排障（PC2 / UVC）

| 现象 | 可能原因 | 处理 |
|------|----------|------|
| `uvc_open: Busy` | **teleimager-server** 等已占用相机 | `sudo systemctl stop teleimager.service`；`pgrep -a teleimager` 确认无进程 |
| `no MJPEG 1856x800` | 运行了**旧二进制** | `make clean && make`，确认源码含 `2560` 优先逻辑 |
| `Corrupt JPEG` / `read frame failed` | **3840×1080@60** 超过 USB2 带宽 | 使用 `--stereo`（已优先 2560×720）；或降 fps |
| `attempt to claim already-claimed interface` | 内核 **uvcvideo** 与 libuvc 争用 | 停 teleimager；必要时避免与其他 UVC 进程并行 |
| `make: getcwd: No such file or directory` | shell 当前目录已被删除 | `cd` 到仓库路径后再 `make` |
| `unsupported descriptor subtype VS_*` | libuvc 解析扩展描述符的警告 | 一般可忽略 |

**与 teleimager 共存**：同一台 PC2 上 **不能** 同时由 teleimager 与本 Sender 打开同一支 UVC 相机；联调 Sender 前请停止 teleimager 服务。

**Python 快速验证相机模式（与 C++ 对齐）**

```bash
python3 -c "
import uvc, cv2
cap = uvc.Capture('1:4')          # 改为你的 uid
cap.frame_mode = (2560, 720, 60, 'MJPG')
for i in range(10):
    f = cap.get_frame_robust()
    img = cv2.imdecode(f, cv2.IMREAD_COLOR)
    print(i, None if img is None else img.shape)
"
```

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
