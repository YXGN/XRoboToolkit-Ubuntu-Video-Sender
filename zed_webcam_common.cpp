/*
 * zed_webcam_common.cpp
 *
 * 实现 USB 摄像头 → BGRA →（可选预览分支）→ x264/x265 → appsink → TCP 封包。
 * --listen 与 --send 仅在「谁写入 current_camera_config / send_to_*」上不同，
 * 推流路径统一为本文件中的 streamingThreadFunction。
 */

#include "zed_webcam_common.hpp"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstring>
#include <vector>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <iostream>
#include <opencv2/opencv.hpp>

/* ========== 全局状态（与 listen/send 共享）========== */

CameraRequestData current_camera_config;

std::atomic<bool> stop_requested{false};
std::atomic<bool> streaming_active{false};
std::atomic<bool> encoding_enabled{false};
std::atomic<bool> send_enabled{false};
std::atomic<bool> preview_enabled{false};

std::mutex config_mutex;
std::condition_variable streaming_cv;
std::mutex streaming_mutex;

std::unique_ptr<TCPClient> sender_ptr;
std::string send_to_server;
int send_to_port = 0;

std::string g_cli_camera_path;
std::string g_cli_stereo_camera_path;

namespace {

/* 推流工作线程句柄；由 streaming_mutex 保护创建与 join */
std::unique_ptr<std::thread> streaming_thread;

/* Ctrl+C：先停编码与 TCP，再让 listen 模块关掉控制口服务端 */
static void zed_webcam_on_sigint(int) {
  std::cout << "\nSIGINT received. Stopping all threads..." << std::endl;
  stop_requested.store(true);
  stopStreamingThread();
  zed_webcam_stop_listen_server();
  streaming_cv.notify_all();
}

/*
 * appsink 回调（运行在 GStreamer 线程）。
 * 每个 buffer 视为一个完整编码访问单元（AU）；前面拼接 4 字节大端无符号长度，
 * 与 main_zed_tcp / Pico 接收侧约定一致。
 */
static GstFlowReturn on_new_sample(GstAppSink *sink, gpointer /*user_data*/) {
  GstSample *sample = gst_app_sink_pull_sample(sink);
  if (!sample)
    return GST_FLOW_ERROR;

  GstBuffer *buffer = gst_sample_get_buffer(sample);
  GstMapInfo map;
  if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
    const uint8_t *data = map.data;
    gsize size = map.size;

    if (send_enabled.load() && sender_ptr && sender_ptr->isConnected() &&
        data && size > 0) {
      try {
        std::vector<uint8_t> packet(4 + size);
        packet[0] = (size >> 24) & 0xFF;
        packet[1] = (size >> 16) & 0xFF;
        packet[2] = (size >> 8) & 0xFF;
        packet[3] = (size)&0xFF;
        std::copy(data, data + size, packet.begin() + 4);

        sender_ptr->sendData(packet);
      } catch (const TCPException &e) {
        std::cerr << "TCP error in on_new_sample: " << e.what() << std::endl;
        streaming_active.store(false);
      } catch (const std::exception &e) {
        std::cerr << "Unexpected error in on_new_sample: " << e.what()
                  << std::endl;
        streaming_active.store(false);
      }
    }

    gst_buffer_unmap(buffer, &map);
  }

  gst_sample_unref(sample);
  return GST_FLOW_OK;
}

/* x264enc.bitrate 单位为 kbps；Open/Pico 侧 bitrate 常为 bps */
static int bitrateBpsToKbps(int bitrate_bps) {
  if (bitrate_bps <= 0)
    return 4000;
  int kbps = bitrate_bps / 1000;
  return std::max(1, kbps);
}

/*
 * 拼装 GStreamer 管线字符串：
 *   appsrc(BGRA, 目标分辨率×fps) → videoconvert → tee
 *     ├─[编码支路]→ x264(I420, Annex B AU) 或 x265 → appsink
 *     └─[可选]→ autovideosink（本地预览）
 * BGRA 先转 I420 再进 x264enc，避免默认成 YUV444/High444 导致部分解码器白屏。
 */
static std::string buildWebcamPipelineString(const CameraRequestData &config,
                                             bool preview) {
  int fps = config.fps > 0 ? config.fps : 30;
  int w = config.width > 0 ? config.width : 2560;
  int h = config.height > 0 ? config.height : 720;
  int bitrate_kbps = bitrateBpsToKbps(config.bitrate);
  /*
   * GOP 设为 fps*4（约 4 秒），减少 I 帧频率从而降低帧大小方差。
   * vbv-buf-capacity 限制编码器 VBV 缓冲为 500 ms，强制近似 CBR，
   * 防止编码器在 I 帧附近爆出大数据包堵塞 TCP 回调线程。
   */
  int key_int_max = std::max(30, fps * 4);

  std::string pipeline_str =
      "appsrc name=mysource is-live=true format=time "
      "caps=video/x-raw,format=BGRA,width=" +
      std::to_string(w) + ",height=" + std::to_string(h) + ",framerate=" +
      std::to_string(fps) + "/1 ! ";

  pipeline_str += "videoconvert ! tee name=t ";

  if (config.enableMvHevc) {
    pipeline_str += "t. ! queue ! x265enc tune=zerolatency bitrate=" +
                    std::to_string(bitrate_kbps) +
                    " speed-preset=ultrafast key-int-max=" +
                    std::to_string(key_int_max) +
                    " ! h265parse ! appsink name=mysink emit-signals=true "
                    "sync=false ";
  } else {
    /*
     * vbv-buf-capacity=500  → VBV 缓冲上限 500 ms，强制编码器平滑输出（近似 CBR）
     * intra-refresh=true    → 用渐进式刷新替代硬性 IDR 帧，彻底消灭帧大小尖峰；
     *                         代价是随机 seek 困难，对实时推流无影响。
     * option-string 中的 nal-hrd=cbr 配合 VBV 让码率控制更严格。
     */
    pipeline_str +=
        "t. ! queue ! videoconvert ! video/x-raw,format=I420 ! "
        "x264enc profile=high tune=zerolatency bitrate=" +
        std::to_string(bitrate_kbps) +
        " speed-preset=ultrafast key-int-max=" +
        std::to_string(key_int_max) +
        " vbv-buf-capacity=500 intra-refresh=true"
        " option-string=\"nal-hrd=cbr\""
        " ! h264parse "
        "! video/x-h264,stream-format=(string)byte-stream,"
        "alignment=(string)au "
        "! appsink name=mysink emit-signals=true sync=false ";
  }

  if (preview) {
    pipeline_str +=
        "t. ! queue ! videoconvert ! autovideosink sync=false ";
  }

  return pipeline_str;
}

/* 探测某 /dev/video 节点能否以 1920×1080 实际采到一帧（用于单目自动选型） */
static bool probe1080pDevice(const std::string &device_path, std::string &err) {
  cv::VideoCapture cap(device_path);
  if (!cap.isOpened()) {
    err = "无法打开";
    return false;
  }
  cap.set(cv::CAP_PROP_FRAME_WIDTH, 1920);
  cap.set(cv::CAP_PROP_FRAME_HEIGHT, 1080);
  cv::Mat frame;
  if (!cap.read(frame) || frame.empty()) {
    err = "read 失败";
    cap.release();
    return false;
  }
  int w = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_WIDTH));
  int h = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_HEIGHT));
  if (frame.cols != 1920 || frame.rows != 1080) {
    err = "实际分辨率 " + std::to_string(frame.cols) + "x" +
          std::to_string(frame.rows);
    cap.release();
    return false;
  }
  if (w != 1920 || h != 1080) {
    err = "属性报告 " + std::to_string(w) + "x" + std::to_string(h);
    cap.release();
    return false;
  }
  cap.release();
  err.clear();
  return true;
}

/* 按 /dev/video 编号升序，取第一个通过 probe1080pDevice 的设备 */
static std::string pickAuto1080pDevice() {
  glob_t gbuf;
  memset(&gbuf, 0, sizeof(gbuf));
  if (glob("/dev/video*", 0, nullptr, &gbuf) != 0) {
    std::cerr << "glob /dev/video* 失败: " << strerror(errno) << std::endl;
    return std::string();
  }

  std::vector<std::pair<int, std::string>> numbered;
  for (size_t i = 0; i < gbuf.gl_pathc; ++i) {
    std::string p = gbuf.gl_pathv[i];
    int n = -1;
    if (std::sscanf(p.c_str(), "/dev/video%d", &n) != 1)
      continue;
    numbered.push_back(std::make_pair(n, p));
  }
  globfree(&gbuf);
  std::sort(numbered.begin(), numbered.end());

  for (const auto &pr : numbered) {
    std::string err;
    if (probe1080pDevice(pr.second, err)) {
      std::cout << "自动选中摄像头: " << pr.second << " (1920x1080)" << std::endl;
      return pr.second;
    } else {
      std::cout << "跳过 " << pr.second << " : " << err << std::endl;
    }
  }
  return std::string();
}

/* 常见双目 MJPEG SBS 模式的采集分辨率（与 v4l2 枚举一致） */
static constexpr int kStereoSbsCaptureWidth = 1856;
static constexpr int kStereoSbsCaptureHeight = 800;

/*
 * 打开 V4L2 摄像头：优先 CAP_V4L2，避免与其他 GStreamer 进程争用默认后端。
 * stereo_sbs：MJPEG + 固定宽高；否则单目 1920×1080。fps 与编码管线一致，用于驱动侧协商。
 */
static bool openUsbCapture(const std::string &device, bool stereo_sbs, int fps,
                           cv::VideoCapture &cap, std::string &err_detail) {
  err_detail.clear();

  auto configure = [&](cv::VideoCapture &c) -> bool {
    if (!c.isOpened())
      return false;
    if (stereo_sbs) {
      c.set(cv::CAP_PROP_FOURCC,
            cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
      c.set(cv::CAP_PROP_FRAME_WIDTH, kStereoSbsCaptureWidth);
      c.set(cv::CAP_PROP_FRAME_HEIGHT, kStereoSbsCaptureHeight);
    } else {
      c.set(cv::CAP_PROP_FRAME_WIDTH, 1920);
      c.set(cv::CAP_PROP_FRAME_HEIGHT, 1080);
    }
    c.set(cv::CAP_PROP_FPS, fps);
    return true;
  };

  cv::VideoCapture tmp;

  int vid = -1;
  if (std::sscanf(device.c_str(), "/dev/video%d", &vid) == 1 && vid >= 0) {
    tmp.open(vid, cv::CAP_V4L2);
    if (configure(tmp)) {
      cap = std::move(tmp);
      return true;
    }
    tmp.release();
  }

  tmp.open(device, cv::CAP_V4L2);
  if (configure(tmp)) {
    cap = std::move(tmp);
    return true;
  }
  tmp.release();

  tmp.open(device);
  if (configure(tmp)) {
    cap = std::move(tmp);
    return true;
  }
  tmp.release();

  int fd = ::open(device.c_str(), O_RDWR | O_NONBLOCK);
  if (fd < 0) {
    err_detail = strerror(errno);
  } else {
    ::close(fd);
    err_detail =
        "OpenCV 仍未打开（底层节点可读）；若需独占请先关掉其它取流进程";
  }
  return false;
}

} // namespace

void zed_webcam_install_sigint_handler() {
  signal(SIGINT, zed_webcam_on_sigint);
}

void zed_webcam_set_preview_enabled(bool v) { preview_enabled.store(v); }

void zed_webcam_set_camera_paths(const std::string &mono,
                                 const std::string &stereo) {
  g_cli_camera_path = mono;
  g_cli_stereo_camera_path = stereo;
}

bool initialize_sender() {
  int retry = 10;
  while (retry > 0 && !sender_ptr && !stop_requested.load()) {
    try {
      sender_ptr = std::unique_ptr<TCPClient>(
          new TCPClient(send_to_server, send_to_port));
      std::cout << "Attempting to connect to " << send_to_server << ":"
                << send_to_port << std::endl;
      sender_ptr->connect();
      return true;
    } catch (const TCPException &e) {
      std::cerr << "Failed to connect to server: " << e.what() << std::endl;
      sender_ptr = nullptr;
    }
    std::this_thread::sleep_for(std::chrono::seconds(1));
    retry--;
  }
  return false;
}

void startStreamingThread() {
  std::lock_guard<std::mutex> lock(streaming_mutex);
  if (streaming_thread && streaming_thread->joinable()) {
    std::cout << "Streaming thread already running" << std::endl;
    return;
  }

  streaming_active.store(true);
  streaming_thread = make_unique_helper<std::thread>(streamingThreadFunction);
  std::cout << "Started streaming thread" << std::endl;
}

void stopStreamingThread() {
  std::lock_guard<std::mutex> lock(streaming_mutex);

  streaming_active.store(false);
  encoding_enabled.store(false);
  send_enabled.store(false);

  if (sender_ptr && sender_ptr->isConnected()) {
    sender_ptr->disconnect();
  }
  sender_ptr = nullptr;

  if (streaming_thread && streaming_thread->joinable()) {
    streaming_cv.notify_all();
    streaming_thread->join();
    streaming_thread = nullptr;
    std::cout << "Stopped streaming thread" << std::endl;
  }
}

/*
 * 推流线程（与主线程、GStreamer 回调线程并发）：
 *  1) TCP 连接接收端
 *  2) 拷贝一份 current_camera_config，确定输出宽高 fps
 *  3) 选择设备：stereo 路径或单目路径（CLI 或自动 1080p）
 *  4) openUsbCapture + parse_launch + PLAYING
 *  5) 循环：read → BGRA →（单目则左右复制）→ resize → 写 PTS/DURATION → appsrc push
 * h264parse 配置为 AU 对齐，故 appsink 单次回调对应一块可独立解码的负载。
 */
void streamingThreadFunction() {
  std::cout << "Streaming thread started" << std::endl;

  try {
    if (!initialize_sender()) {
      std::cerr << "Failed to initialize sender, streaming thread stopping"
                << std::endl;
      return;
    }

    encoding_enabled.store(true);
    send_enabled.store(true);

    CameraRequestData config;
    {
      std::lock_guard<std::mutex> lock(config_mutex);
      config = current_camera_config;
    }

    int out_w = config.width > 0 ? config.width : 2560;
    int out_h = config.height > 0 ? config.height : 720;
    int fps = config.fps > 0 ? config.fps : 30;

    bool use_stereo_sbs = !g_cli_stereo_camera_path.empty();
    std::string device = use_stereo_sbs ? g_cli_stereo_camera_path
                                        : (g_cli_camera_path.empty()
                                               ? pickAuto1080pDevice()
                                               : g_cli_camera_path);
    if (device.empty()) {
      std::cerr << "未找到可用摄像头设备。请使用 --camera 或 --stereo-camera 指定"
                << std::endl;
      return;
    }
    std::cout << "采集模式: " << (use_stereo_sbs ? "stereo-sbs" : "mono-copy")
              << "，设备: " << device << std::endl;

    cv::VideoCapture cap;
    std::string open_err;
    if (!openUsbCapture(device, use_stereo_sbs, fps, cap, open_err)) {
      std::cerr << "无法打开摄像头: " << device;
      if (!open_err.empty())
        std::cerr << " — " << open_err;
      std::cerr << std::endl;
      return;
    }

    std::string pipeline_str =
        buildWebcamPipelineString(config, preview_enabled.load());
    std::cout << "Webcam pipeline: " << pipeline_str << std::endl;

    GError *error = nullptr;
    GstElement *pipeline = gst_parse_launch(pipeline_str.c_str(), &error);
    if (!pipeline) {
      std::cerr << "Failed to create pipeline: "
                << (error ? error->message : "?") << std::endl;
      g_clear_error(&error);
      return;
    }

    GstElement *appsrc = gst_bin_get_by_name(GST_BIN(pipeline), "mysource");
    GstElement *appsink = gst_bin_get_by_name(GST_BIN(pipeline), "mysink");
    if (!appsrc || !appsink) {
      std::cerr << "Failed to get appsrc/appsink" << std::endl;
      if (appsrc)
        gst_object_unref(appsrc);
      if (appsink)
        gst_object_unref(appsink);
      gst_object_unref(pipeline);
      return;
    }

    g_signal_connect(appsink, "new-sample", G_CALLBACK(on_new_sample), nullptr);

    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    cv::Mat frame;
    int frame_id = 0;

    std::cout << "Starting webcam streaming loop..." << std::endl;
    while (streaming_active.load() && !stop_requested.load()) {
      if (!cap.read(frame) || frame.empty()) {
        std::cerr << "read frame failed" << std::endl;
        break;
      }

      cv::Mat bgra;
      if (frame.channels() == 4) {
        bgra = frame;
      } else if (frame.channels() == 3) {
        cv::cvtColor(frame, bgra, cv::COLOR_BGR2BGRA);
      } else {
        std::cerr << "unsupported channels: " << frame.channels()
                  << std::endl;
        break;
      }

      cv::Mat side_by_side;
      if (use_stereo_sbs) {
        /* 设备已输出左右并排，仅缩放至 Pico 请求分辨率 */
        side_by_side = bgra;
        if (side_by_side.cols < 2 * side_by_side.rows) {
          std::cout << "[warn] stereo-sbs 模式下输入宽高比偏小: "
                    << side_by_side.cols << "x" << side_by_side.rows
                    << "，请确认设备输出是否为左右拼接" << std::endl;
        }
      } else {
        /* 单目模拟 ZED SIDE_BY_SIDE：复制左右两半 */
        cv::hconcat(bgra, bgra, side_by_side);
      }

      cv::Mat out_bgra;
      cv::resize(side_by_side, out_bgra, cv::Size(out_w, out_h), 0, 0,
                 cv::INTER_LINEAR);

      if (!encoding_enabled.load())
        break;

      size_t frame_bytes =
          static_cast<size_t>(out_bgra.total() * out_bgra.elemSize());
      GstBuffer *buffer =
          gst_buffer_new_allocate(nullptr, frame_bytes, nullptr);
      GstMapInfo map;
      if (!gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
        gst_buffer_unref(buffer);
        break;
      }
      memcpy(map.data, out_bgra.data, frame_bytes);
      gst_buffer_unmap(buffer, &map);

      /* 运行时钟：按 fps 均匀递增，与 live appsrc 协商一致 */
      GST_BUFFER_PTS(buffer) =
          gst_util_uint64_scale(frame_id, GST_SECOND, fps);
      GST_BUFFER_DURATION(buffer) =
          gst_util_uint64_scale(1, GST_SECOND, fps);

      GstFlowReturn ret =
          gst_app_src_push_buffer(GST_APP_SRC(appsrc), buffer);
      if (ret != GST_FLOW_OK) {
        std::cerr << "appsrc push failed: " << ret << std::endl;
        break;
      }

      frame_id++;
    }

    std::cout << "Streaming loop ended, cleaning up..." << std::endl;

    gst_app_src_end_of_stream(GST_APP_SRC(appsrc));
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(appsrc);
    gst_object_unref(appsink);
    gst_object_unref(pipeline);
    cap.release();

  } catch (const std::exception &e) {
    std::cerr << "Streaming thread error: " << e.what() << std::endl;
  }

  std::cout << "Streaming thread finished" << std::endl;
}
