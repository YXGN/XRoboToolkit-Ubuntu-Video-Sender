# 视频编码与 TCP 推流管线说明（zed_webcam_common）

本文说明 **`zed_webcam_common.cpp`** 中从 **`appsrc`** 经 **x264enc / x265enc** 到 **`appsink`**，再封装为 **`[4 字节大端长度][payload]`** 经 **TCP** 发出的实现要点。与 **`README_design.md`** 中的心智图一致，侧重管线细节与接收端对齐注意事项。

---

## 1. 总体数据流

```text
OpenCV 采集线程 (streamingThreadFunction)
  → BGRA 帧 (resize 至目标宽高)
  → gst_app_src_push_buffer(appsrc)

GStreamer 内部线程
  → videoconvert → [tee] → queue → 编码支路 → appsink

appsink 回调 (on_new_sample，GStreamer 线程)
  → 拼 [4B BE uint32 长度][编码负载]
  → TCPClient::sendData
```

- **喂入**：应用线程 **`gst_app_src_push_buffer`**。  
- **编码与拉取**：GStreamer 调度；**`on_new_sample`** 在 **另一线程** 执行，与 `push_buffer` 并发。  
- **网络**：每个逻辑包 = **固定 4 字节大端无符号长度** + **紧随其后的 N 字节**（无额外帧头）。

实现入口：`buildWebcamPipelineString`、`streamingThreadFunction`、`on_new_sample`（见 `zed_webcam_common.cpp`）。

---

## 2. 管线字符串结构（`buildWebcamPipelineString`）

### 2.1 公共前缀

| 片段 | 含义 |
|------|------|
| `appsrc name=mysource is-live=true format=time` | 应用侧推流；**直播模式**；缓冲带 **PTS/DURATION**（`format=time`）。 |
| `caps=video/x-raw,format=BGRA,width=×,height=×,framerate=fps/1` | 未压缩输入：**BGRA**、输出分辨率与帧率（来自 `CameraRequestData`：Pico `OPEN_CAMERA` 或 `--send` CLI）。 |
| `videoconvert ! tee name=t` | 色彩/格式适配；**tee** 分出编码支路与可选预览支路。 |

### 2.2 编码支路（从 `t.` 引出）

**H.264（默认，`enableMvHevc == 0`）**

```text
t. ! queue ! videoconvert ! video/x-raw,format=I420 !
    x264enc profile=high tune=zerolatency bitrate=<kbps> speed-preset=ultrafast key-int-max=<GOP> !
    h264parse !
    video/x-h264,stream-format=(string)byte-stream,alignment=(string)au !
    appsink name=mysink emit-signals=true sync=false
```

| 元件 | 作用 |
|------|------|
| `queue` | 小缓冲，解耦采集与编码。 |
| `videoconvert → I420` | 显式 **4:2:0**；避免 BGRA 直入 x264 走成 **YUV444 / High444** 导致部分解码器白屏。 |
| `x264enc` | 软件编码；`tune=zerolatency` 偏向低延迟；`bitrate` 为 **kbps**（源码中由 **bps ÷ 1000** 换算）。 |
| `h264parse` + caps | **`stream-format=byte-stream`**：**Annex B**（起始码分隔的 NAL 流）。**`alignment=au`**：尽量 **一 Access Unit（AU）一 buffer**，便于下游按「长度前缀 + 一块 AU」处理。 |
| `appsink` | **`emit-signals=true`**：每块缓冲触发 **`new-sample`**；**`sync=false`**：不按时钟回放，尽快交付应用。 |

**HEVC（`enableMvHevc != 0`）**

```text
t. ! queue ! x265enc tune=zerolatency bitrate=<kbps> ... !
    h265parse ! appsink name=mysink emit-signals=true sync=false
```

未再附加与 H.264 相同的 `stream-format`/`alignment` caps；负载形态由 **`h265parse`** 与插件协商决定。应用侧仍按 **「4 字节长度 + 紧随 payload」** 发送，与 H.264 路径封包方式一致。

### 2.3 可选预览支路

当 **`preview_enabled`** 为真时追加：

```text
t. ! queue ! videoconvert ! autovideosink sync=false
```

与编码支路 **并行**，不替代编码。

---

## 3. `appsrc` 侧：谁在推、时间戳含义

在 **`streamingThreadFunction`** 中：

1. OpenCV **`cap.read`** → 转 **BGRA** →（单目 **`hconcat` 复制成双路** 或 SBS 直通）→ **`resize`** 到 `config.width × config.height`。  
2. 分配 **`GstBuffer`**，将像素拷入 buffer。  
3. 设置 **`GST_BUFFER_PTS`** = `frame_id * (1/fps)`，`**GST_BUFFER_DURATION**` = `1/fps`（以 `gst_util_uint64_scale` 按 `GST_SECOND` 与 fps 计算）。  
4. **`gst_app_src_push_buffer(GST_APP_SRC(appsrc), buffer)`**。

这样 **`appsrc` 的 caps（framerate）** 与 **每帧 PTS/DURATION** 一致，便于 live 链路协商。

---

## 4. `appsink` 侧：`on_new_sample` 与 TCP 封包

1. **`gst_app_sink_pull_sample`** 取出一个 **`GstSample`**，内含编码后的 **`GstBuffer`**。  
2. **`gst_buffer_map`** 得到 **`data`** 与 **`size`**（**仅 payload**，不含本协议的长度前缀）。  
3. 若 **`send_enabled`** 且 **`TCPClient`** 已连接：  
   - 分配 **`4 + size`** 字节；  
   - **`packet[0..3]`**：将 **`size`** 写作 **32 位无符号整数、大端（高字节在前）**；  
   - **`packet[4..]`**：拷贝 **`data[0..size)`**；  
   - **`sender_ptr->sendData(packet)`** 整包写入 TCP。

**TCP 字节流语义**（重复多帧时）：

```text
(len_be32)(payload)(len_be32)(payload)...
```

其中 **`len_be32`** 的值等于 **紧跟其后的 `payload` 的字节数**。

---

## 5. 线程关系（调试延迟时需注意）

| 线程 | 工作 |
|------|------|
| **`streamingThreadFunction`** | 读摄像头、`push_buffer` → `appsrc`。 |
| **GStreamer 内部** | `appsrc` → 编码 → `appsink`。 |
| **`on_new_sample`（GStreamer 回调线程）** | `pull_sample`、组包、`sendData`。 |

因此 **「采集时刻」与「TCP 发出时刻」** 不在同一线程；若要做端到端延迟打点，需用 **线程安全** 结构或时间戳关联（例如用 PTS 或 `frame_id` 对齐）。

---

## 6. 接收端对齐清单

1. **读 4 字节**：按 **大端** 解析为 **`uint32_t N`**。  
2. **再读 `N` 字节**：作为一帧逻辑负载交给解码器。  
3. **H.264**：期望 **Annex B（byte-stream）**；若解码器只支持 **AVCC**，需在接收端做转换（本 Sender 不输出 AVCC）。  
4. **HEVC**：需与 **`h265parse` 输出** 及解码器约定一致；封包仍是 **4 字节长度 + 负载**。  
5. **TCP**：为 **字节流**；若 `recv` 一次未收满 `4+N`，接收端必须 **缓存拼包** 再解析。

---

## 7. 相关文件

| 文件 | 内容 |
|------|------|
| `zed_webcam_common.cpp` | `buildWebcamPipelineString`、`streamingThreadFunction`、`on_new_sample` |
| `zed_webcam_common.hpp` | `CameraRequestData`、全局状态声明 |
| `network_helper.hpp` | `TCPClient::sendData`（循环 `send` 直至发完） |
| `README_design.md` | 模块划分、`--listen` / `--send` 心智图 |

---

## 8. 本地打印完整管线字符串

运行 **`OrinVideoSender`** 开始推流时，日志会打印 **`Webcam pipeline:`** 后跟 **`gst_parse_launch` 使用的整行字符串**，便于与 `gst-launch-1.0` 对照调试。
