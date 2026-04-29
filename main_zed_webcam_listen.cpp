/*
 * main_zed_webcam_listen.cpp
 *
 * 用途（中文说明）：
 * 在无 NVIDIA CUDA、无 Stereolabs ZED SDK 的 Ubuntu 上，复刻 Pico XRoboToolkit 与
 * OrinVideoSender 之间的「控制 + 视频」协议：本进程在 --listen 地址上作为 TCP 服务端，
 * 接收 OPEN_CAMERA / CLOSE_CAMERA；收到 OPEN_CAMERA 后按载荷中的 ip:port 作为 TCP 客户端，
 * 推送与 main_zed_tcp 相同的「4 字节大端长度 + H.264/H.265 负载」。
 *
 * 视频源：USB 摄像头。可选 --camera 指定设备；否则按 /dev/video 编号升序选取「首个能
 * 以 1920×1080 采到一帧」的设备。将单目画面左右复制成「伪双目」并排帧，再缩放到
 * OPEN_CAMERA 请求的分辨率（BGRA），经 GStreamer 软件编码器输出，供头显解码。
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <errno.h>
#include <glob.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <opencv2/opencv.hpp>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "network_helper.hpp"

// ============================================================================
// 一、与 Pico 对齐的网络协议结构体（字节布局须与 Unity/C# 端一致，勿改）
// ============================================================================

struct CameraRequestData {
  int width;
  int height;
  int fps;
  int bitrate;
  int enableMvHevc;
  int renderMode;
  int port;
  std::string camera;
  std::string ip;

  CameraRequestData()
      : width(0), height(0), fps(0), bitrate(0), enableMvHevc(0), renderMode(0),
        port(0) {}
};

struct NetworkDataProtocol {
  std::string command;
  int length;
  std::vector<uint8_t> data;

  NetworkDataProtocol() : length(0) {}
  NetworkDataProtocol(const std::string &cmd, const std::vector<uint8_t> &d)
      : command(cmd), length(static_cast<int>(d.size())), data(d) {}
};

// OPEN_CAMERA 载荷反序列化：魔数 0xCAFE、版本号、小端 int32 字段 + 紧凑字符串
class CameraRequestDeserializer {
public:
  static CameraRequestData deserialize(const std::vector<uint8_t> &data) {
    if (data.size() < 10) {
      throw std::invalid_argument("Data is too small for valid camera request");
    }

    size_t offset = 0;

    if (data[offset] != 0xCA || data[offset + 1] != 0xFE) {
      throw std::invalid_argument("Invalid magic bytes");
    }
    offset += 2;

    uint8_t version = data[offset++];
    if (version != 1) {
      throw std::invalid_argument("Unsupported protocol version");
    }

    CameraRequestData result;

    if (offset + 28 > data.size()) {
      throw std::invalid_argument("Data too small for integer fields");
    }

    result.width = readInt32(data, offset);
    result.height = readInt32(data, offset + 4);
    result.fps = readInt32(data, offset + 8);
    result.bitrate = readInt32(data, offset + 12);
    result.enableMvHevc = readInt32(data, offset + 16);
    result.renderMode = readInt32(data, offset + 20);
    result.port = readInt32(data, offset + 24);
    offset += 28;

    result.camera = readCompactString(data, offset);
    result.ip = readCompactString(data, offset);

    return result;
  }

private:
  static int32_t readInt32(const std::vector<uint8_t> &data, size_t offset) {
    if (offset + 4 > data.size()) {
      throw std::out_of_range("Not enough data to read int32");
    }
    // 小端，与 C# BitConverter 默认一致
    return static_cast<int32_t>((data[offset]) | (data[offset + 1] << 8) |
                                (data[offset + 2] << 16) |
                                (data[offset + 3] << 24));
  }

  static std::string readCompactString(const std::vector<uint8_t> &data,
                                       size_t &offset) {
    if (offset >= data.size()) {
      throw std::out_of_range("Not enough data to read string length");
    }

    uint8_t length = data[offset++];
    if (length == 0) {
      return std::string();
    }

    if (offset + length > data.size()) {
      throw std::out_of_range("Not enough data to read string content");
    }

    std::string result(reinterpret_cast<const char *>(&data[offset]), length);
    offset += length;
    return result;
  }
};

// 外层命令：命令名字符串长度 + 命令体 + 数据长度 + 数据（小端长度字段）
class NetworkDataProtocolDeserializer {
public:
  static NetworkDataProtocol deserialize(const std::vector<uint8_t> &buffer) {
    if (buffer.size() < 8) {
      throw std::invalid_argument("Buffer too small for valid protocol data");
    }

    size_t offset = 0;

    int32_t commandLength = readInt32(buffer, offset);
    offset += 4;

    if (commandLength < 0 || offset + commandLength > buffer.size()) {
      throw std::invalid_argument("Invalid command length");
    }

    std::string command;
    if (commandLength > 0) {
      command = std::string(reinterpret_cast<const char *>(&buffer[offset]),
                            commandLength);
      size_t nullPos = command.find('\0');
      if (nullPos != std::string::npos) {
        command = command.substr(0, nullPos);
      }
    }
    offset += commandLength;

    if (offset + 4 > buffer.size()) {
      throw std::invalid_argument("Buffer too small for data length");
    }

    int32_t dataLength = readInt32(buffer, offset);
    offset += 4;

    if (dataLength < 0 || offset + dataLength > buffer.size()) {
      throw std::invalid_argument("Invalid data length");
    }

    std::vector<uint8_t> data;
    if (dataLength > 0) {
      data.assign(buffer.begin() + offset,
                  buffer.begin() + offset + dataLength);
    }

    return NetworkDataProtocol(command, data);
  }

private:
  static int32_t readInt32(const std::vector<uint8_t> &data, size_t offset) {
    if (offset + 4 > data.size()) {
      throw std::out_of_range("Not enough data to read int32");
    }
    return static_cast<int32_t>((data[offset]) | (data[offset + 1] << 8) |
                                (data[offset + 2] << 16) |
                                (data[offset + 3] << 24));
  }
};

// ============================================================================
// 二、全局状态与网络组件
// ============================================================================

CameraRequestData current_camera_config;

std::atomic<bool> stop_requested{false};
std::atomic<bool> streaming_active{false};
std::atomic<bool> encoding_enabled{false};
std::atomic<bool> send_enabled{false};
std::atomic<bool> preview_enabled{false};

std::unique_ptr<std::thread> listen_thread;
std::unique_ptr<std::thread> streaming_thread;
std::mutex config_mutex;
std::condition_variable streaming_cv;
std::mutex streaming_mutex;

std::unique_ptr<TCPClient> sender_ptr;
std::unique_ptr<TCPServer> server_ptr;
std::string send_to_server = "";
int send_to_port = 0;

// 命令行：若非空则强制使用该 V4L2 设备；否则自动枚举首个 1920x1080 可用节点
static std::string g_cli_camera_path;

// ---------------------------------------------------------------------------
// 实验选项（Pico 白屏 / 码流格式联调）：中文说明见 --help
// ---------------------------------------------------------------------------
// h264parse 之后、appsink 之前是否强制 caps：空=不强制（与原先一致，由元素协商）
// 非空时仅允许 "byte-stream"（Annex B 起始码）或 "avc"（长度前缀 NAL，AVCC 一类）
static std::string g_h264_out_format;

// 若非空且当前为 H.264（非 HEVC），将 appsink 收到的每帧负载原样追加写入该路径；
// 内容与发往 TCP 的「去掉 4 字节大端长度前缀后的 payload」一致，便于 ffprobe/ffplay 自证
static std::string g_dump_h264_path;
static std::mutex g_dump_h264_mutex;
static FILE *g_dump_h264_fp = nullptr;

static void closeH264DumpFile() {
  std::lock_guard<std::mutex> lk(g_dump_h264_mutex);
  if (g_dump_h264_fp) {
    fclose(g_dump_h264_fp);
    g_dump_h264_fp = nullptr;
  }
}

template <typename T, typename... Args>
std::unique_ptr<T> make_unique_helper(Args &&...args) {
  return std::unique_ptr<T>(new T(std::forward<Args>(args)...));
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

void handleOpenCamera(const std::vector<uint8_t> &data);
void handleCloseCamera(const std::vector<uint8_t> &data);
void startStreamingThread();
void stopStreamingThread();
void streamingThreadFunction();
void listenThreadFunction(const std::string &listen_address);

// ============================================================================
// 三、Pico 控制通道：外层 4 字节大端 body 长度 + NetworkDataProtocol
// ============================================================================

void onDataCallback(const std::string &command) {
  std::vector<uint8_t> binaryData(command.begin(), command.end());

  for (size_t i = 0; i < std::min(binaryData.size(), size_t(32)); ++i) {
    std::cout << std::hex << std::setfill('0') << std::setw(2)
              << static_cast<unsigned int>(binaryData[i]) << " ";
  }
  std::cout << std::dec << std::endl;

  if (binaryData.size() < 4) {
    std::cerr << "Data too small to contain length header" << std::endl;
    return;
  }

  // 前 4 字节：body 长度，大端（与 main_zed_tcp 一致）
  uint32_t bodyLength = (static_cast<uint32_t>(binaryData[0]) << 24) |
                        (static_cast<uint32_t>(binaryData[1]) << 16) |
                        (static_cast<uint32_t>(binaryData[2]) << 8) |
                        static_cast<uint32_t>(binaryData[3]);

  if (4 + bodyLength > binaryData.size()) {
    std::cerr << "Data too small for declared body length. Expected: "
              << (4 + bodyLength) << ", got: " << binaryData.size()
              << std::endl;
    return;
  }

  std::vector<uint8_t> protocolData(binaryData.begin() + 4,
                                    binaryData.begin() + 4 + bodyLength);

  for (size_t i = 0; i < std::min(protocolData.size(), size_t(32)); ++i) {
    std::cout << std::hex << std::setfill('0') << std::setw(2)
              << static_cast<unsigned int>(protocolData[i]) << " ";
  }
  std::cout << std::dec << std::endl;

  try {
    if (protocolData.size() >= 8) {
      int32_t cmdLen = (protocolData[0]) | (protocolData[1] << 8) |
                       (protocolData[2] << 16) | (protocolData[3] << 24);
      int32_t dataLenPos = 4 + cmdLen;
      std::cout << "Command length: " << cmdLen << std::endl;
      if (static_cast<size_t>(dataLenPos + 4) <= protocolData.size()) {
        int32_t dataLen = (protocolData[dataLenPos]) |
                          (protocolData[dataLenPos + 1] << 8) |
                          (protocolData[dataLenPos + 2] << 16) |
                          (protocolData[dataLenPos + 3] << 24);
        std::cout << "Data length: " << dataLen << std::endl;
      }
    }

    NetworkDataProtocol protocol =
        NetworkDataProtocolDeserializer::deserialize(protocolData);

    std::cout << "Received protocol command: '" << protocol.command
              << "' (length: " << protocol.command.length() << ")" << std::endl;

    if (protocol.command == "OPEN_CAMERA") {
      handleOpenCamera(protocol.data);
    } else if (protocol.command == "CLOSE_CAMERA") {
      handleCloseCamera(protocol.data);
    } else {
      std::cout << "Unknown protocol command: " << protocol.command
                << std::endl;
    }
    return;
  } catch (const std::exception &e) {
    std::cout << "Failed to parse as NetworkDataProtocol: " << e.what()
              << std::endl;
  }
}

void onDisconnectCallback() {
  std::cout << "Client disconnected, stopping streaming" << std::endl;
  stopStreamingThread();
}

void listenThreadFunction(const std::string &listen_address) {
  std::cout << "Listen thread started on " << listen_address << std::endl;

  while (!stop_requested.load()) {
    try {
      server_ptr = make_unique_helper<TCPServer>(listen_address);
      server_ptr->setDataCallback(onDataCallback);
      server_ptr->setDisconnectCallback(onDisconnectCallback);
      server_ptr->start();
      std::cout << "TCPServer is listening on " << listen_address << std::endl;

      while (!stop_requested.load() && server_ptr) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }

      if (server_ptr) {
        server_ptr->stop();
        server_ptr = nullptr;
      }

      if (!stop_requested.load()) {
        std::cout << "Waiting for new connection..." << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(1));
      }

    } catch (const std::exception &e) {
      std::cerr << "Listen thread error: " << e.what() << std::endl;
      if (!stop_requested.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
      }
    }
  }

  std::cout << "Listen thread stopped" << std::endl;
}

void handle_sigint(int) {
  std::cout << "\nSIGINT received. Stopping all threads..." << std::endl;
  stop_requested.store(true);

  stopStreamingThread();

  if (server_ptr) {
    server_ptr->stop();
    server_ptr = nullptr;
  }

  streaming_cv.notify_all();
}

// ============================================================================
// 四、编码后输出：与 main_zed_tcp 相同的大端 4 字节长度 + 码流
// ============================================================================

GstFlowReturn on_new_sample(GstAppSink *sink, gpointer /*user_data*/) {
  GstSample *sample = gst_app_sink_pull_sample(sink);
  if (!sample)
    return GST_FLOW_ERROR;

  GstBuffer *buffer = gst_sample_get_buffer(sample);
  GstMapInfo map;
    if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
    const uint8_t *data = map.data;
    gsize size = map.size;

    // 实验：可选落盘（与 TCP 负载同一段字节，不含 4 字节长度头）
    if (!g_dump_h264_path.empty() && data && size > 0) {
      std::lock_guard<std::mutex> lk(g_dump_h264_mutex);
      if (g_dump_h264_fp) {
        if (fwrite(data, 1, size, g_dump_h264_fp) != size)
          std::cerr << "实验落盘：写入字节数与 buffer 长度不一致" << std::endl;
        fflush(g_dump_h264_fp);
      }
    }

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

// ============================================================================
// 五、OPEN_CAMERA / CLOSE_CAMERA（不因 camera 字符串拒绝，仅打日志）
// ============================================================================

void handleOpenCamera(const std::vector<uint8_t> &data) {
  std::cout << "Handling OPEN_CAMERA command" << std::endl;

  try {
    CameraRequestData cameraConfig =
        CameraRequestDeserializer::deserialize(data);

    std::cout << "[Pico] OPEN_CAMERA 解析成功 —— 宽: " << cameraConfig.width
              << " 高: " << cameraConfig.height << " fps: " << cameraConfig.fps
              << " bitrate(bps): " << cameraConfig.bitrate
              << " mvHevc: " << cameraConfig.enableMvHevc
              << " renderMode: " << cameraConfig.renderMode
              << " 视频回连端口: " << cameraConfig.port
              << " 头显声明相机类型(原样): \"" << cameraConfig.camera << "\""
              << " 回连IP: " << cameraConfig.ip << std::endl;

    // 不因非 "ZED" 拒绝：Pico 可能发 ZEDMINI 等，联调后再收紧白名单即可
    {
      std::lock_guard<std::mutex> lock(config_mutex);
      current_camera_config = cameraConfig;
    }

    send_to_server = cameraConfig.ip;
    send_to_port = cameraConfig.port;

    std::cout << "Updated sender target to " << send_to_server << ":"
              << send_to_port << std::endl;

    startStreamingThread();

  } catch (const std::exception &e) {
    std::cerr << "Failed to parse camera config: " << e.what() << std::endl;
    if (!send_to_server.empty() && send_to_port > 0) {
      startStreamingThread();
    } else {
      std::cerr
          << "No valid server configuration available, cannot start streaming"
          << std::endl;
    }
  }
}

void handleCloseCamera(const std::vector<uint8_t> & /*data*/) {
  std::cout << "Handling CLOSE_CAMERA command" << std::endl;
  stopStreamingThread();
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
  // 确保推流线程异常退出时也会关掉实验落盘文件
  closeH264DumpFile();
}

// ============================================================================
// 六、GStreamer：CPU 软件编码（x264 / x265），与 appsrc BGRA caps 对齐
// ============================================================================

// Jetson 上 nvv4l2h264enc 的 bitrate 为 bps；x264enc 的 bitrate= 一般为 kbps
static int bitrateBpsToKbps(int bitrate_bps) {
  if (bitrate_bps <= 0)
    return 4000;
  int kbps = bitrate_bps / 1000;
  return std::max(1, kbps);
}

static std::string buildWebcamPipelineString(const CameraRequestData &config,
                                             bool preview) {
  int fps = config.fps > 0 ? config.fps : 30;
  int w = config.width > 0 ? config.width : 2560;
  int h = config.height > 0 ? config.height : 720;
  int bitrate_kbps = bitrateBpsToKbps(config.bitrate);
  // 关键帧间隔：与 fps 成比例，避免过小导致码流波动过大
  int key_int_max = std::max(15, fps * 2);

  std::string pipeline_str =
      "appsrc name=mysource is-live=true format=time "
      "caps=video/x-raw,format=BGRA,width=" +
      std::to_string(w) + ",height=" + std::to_string(h) + ",framerate=" +
      std::to_string(fps) + "/1 ! ";

  pipeline_str += "videoconvert ! tee name=t ";

  if (config.enableMvHevc) {
    // HEVC：需 gst-plugins-bad 中 x265enc；若环境缺失会在 parse_launch 失败
    pipeline_str += "t. ! queue ! x265enc tune=zerolatency bitrate=" +
                    std::to_string(bitrate_kbps) +
                    " speed-preset=ultrafast key-int-max=" +
                    std::to_string(key_int_max) +
                    " ! h265parse ! appsink name=mysink emit-signals=true "
                    "sync=false ";
  } else {
    pipeline_str += "t. ! queue ! x264enc tune=zerolatency bitrate=" +
                    std::to_string(bitrate_kbps) +
                    " speed-preset=ultrafast key-int-max=" +
                    std::to_string(key_int_max) + " ! h264parse ";
    // 实验：强制下游 caps，便于对比 Jetson 硬件码流在 MediaCodec 侧的假设（byte-stream vs avc）
    if (g_h264_out_format == "byte-stream") {
      pipeline_str +=
          "! video/x-h264,stream-format=(string)byte-stream,"
          "alignment=(string)au ";
    } else if (g_h264_out_format == "avc") {
      pipeline_str +=
          "! video/x-h264,stream-format=(string)avc,alignment=(string)au ";
    }
    pipeline_str +=
        "! appsink name=mysink emit-signals=true sync=false ";
  }

  if (preview) {
    pipeline_str +=
        "t. ! queue ! videoconvert ! autovideosink sync=false ";
  }

  return pipeline_str;
}

// ============================================================================
// 七、USB 设备：自动枚举「编号最小且可 1920x1080 采一帧」的 /dev/video*
// ============================================================================

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
    }
    std::cout << "跳过 " << pr.second << " : " << err << std::endl;
  }
  return std::string();
}

// ============================================================================
// 八、推流线程：OpenCV 采集 -> 左右复制 -> 缩放到 Pico 请求分辨率 -> appsrc
// ============================================================================

void streamingThreadFunction() {
  std::cout << "Streaming thread started" << std::endl;

  // 推流线程任意退出路径都关闭实验落盘句柄，避免句柄泄漏或二次 fopen 未定义行为
  struct H264DumpSessionGuard {
    ~H264DumpSessionGuard() { closeH264DumpFile(); }
  } dump_session_guard;

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

    std::string device =
        g_cli_camera_path.empty() ? pickAuto1080pDevice() : g_cli_camera_path;
    if (device.empty()) {
      std::cerr << "未找到可用的 1920x1080 USB 摄像头，且未指定 --camera"
                << std::endl;
      return;
    }

    cv::VideoCapture cap(device);
    if (!cap.isOpened()) {
      std::cerr << "无法打开摄像头: " << device << std::endl;
      return;
    }
    cap.set(cv::CAP_PROP_FRAME_WIDTH, 1920);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, 1080);
    cap.set(cv::CAP_PROP_FPS, fps);

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

    // 实验：本轮会话以截断写打开；仅 H.264 分支有意义（HEVC 时 appsink 非 H.264，不落盘）
    if (!g_dump_h264_path.empty() && !config.enableMvHevc) {
      std::lock_guard<std::mutex> lk(g_dump_h264_mutex);
      if (g_dump_h264_fp) {
        fclose(g_dump_h264_fp);
        g_dump_h264_fp = nullptr;
      }
      g_dump_h264_fp = fopen(g_dump_h264_path.c_str(), "wb");
      if (g_dump_h264_fp)
        std::cout << "实验：H.264 裸码流将同步写入 " << g_dump_h264_path
                  << "（与 TCP 每帧 payload 相同，无 4 字节大端长度前缀）"
                  << std::endl;
      else
        std::cerr << "实验：无法打开落盘文件: " << g_dump_h264_path
                  << std::endl;
    }

    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    cv::Mat frame1080;
    int frame_id = 0;

    std::cout << "Starting webcam streaming loop..." << std::endl;
    while (streaming_active.load() && !stop_requested.load()) {
      if (!cap.read(frame1080) || frame1080.empty()) {
        std::cerr << "read frame failed" << std::endl;
        break;
      }

      cv::Mat bgra1080;
      if (frame1080.channels() == 4) {
        // 多数驱动在 BGRA 下已可直接使用
        bgra1080 = frame1080;
      } else if (frame1080.channels() == 3) {
        cv::cvtColor(frame1080, bgra1080, cv::COLOR_BGR2BGRA);
      } else {
        std::cerr << "unsupported channels: " << frame1080.channels()
                  << std::endl;
        break;
      }

      // 左右同一画面拼接，模拟 ZED SIDE_BY_SIDE 供头显侧「立体」管线消费
      cv::Mat side_by_side;
      cv::hconcat(bgra1080, bgra1080, side_by_side);

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

// ============================================================================
// 九、程序入口：仅支持 --listen（与 Pico 联调）；可选 --preview / --camera
// ============================================================================

int main(int argc, char *argv[]) {
  gst_init(&argc, &argv);

  bool preview_enabled_local = false;
  bool listen_enabled = false;
  std::string listen_address = "";

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--preview") {
      preview_enabled_local = true;
    } else if (arg == "--listen" && i + 1 < argc) {
      listen_enabled = true;
      listen_address = argv[++i];
    } else if (arg == "--camera" && i + 1 < argc) {
      g_cli_camera_path = argv[++i];
    } else if (arg == "--dump-h264" && i + 1 < argc) {
      // 联调：把编码器输出写到本地，便于 ffprobe/ffplay 验证是否可解码
      g_dump_h264_path = argv[++i];
    } else if (arg == "--h264-format" && i + 1 < argc) {
      // 联调：h264parse 后强制 byte-stream（Annex B）或 avc（AVCC），default 表示不强制 caps
      std::string v = argv[++i];
      if (v == "default" || v == "none" || v == "")
        g_h264_out_format.clear();
      else if (v == "byte-stream" || v == "avc")
        g_h264_out_format = v;
      else {
        std::cerr << "错误：--h264-format 仅支持 default | byte-stream | avc，收到: "
                  << v << std::endl;
        return -1;
      }
    } else if (arg == "--help") {
      std::cout << "用法: " << argv[0] << " [选项]\n";
      std::cout << "  --listen IP:PORT  必选：控制通道监听地址（Pico 连接此端口发 OPEN_CAMERA）\n";
      std::cout << "  --preview         可选：本机 GStreamer 预览窗口\n";
      std::cout << "  --camera PATH     可选：强制使用指定设备，如 /dev/video0；不设则自动选首个 1920x1080\n";
      std::cout << "  --dump-h264 PATH  可选（实验）：H.264 时将 appsink 负载写入文件（与 TCP payload 一致）\n";
      std::cout << "  --h264-format F   可选（实验）：default | byte-stream | avc，控制 h264parse 后强制 caps\n";
      std::cout << "  --help            显示本帮助\n";
      std::cout << "\n说明：本二进制无 ZED SDK，用 USB 摄像头伪装 ZED 侧协议供 XRoboToolkit 使用。\n";
      return 0;
    }
  }

  if (!listen_enabled) {
    std::cerr << "错误：必须指定 --listen IP:PORT（例如 --listen 0.0.0.0:13579）"
              << std::endl;
    return -1;
  }

  signal(SIGINT, handle_sigint);

  preview_enabled.store(preview_enabled_local);

  std::cout << "Starting threaded video streaming server (webcam/ZED-protocol)..."
            << std::endl;

  listen_thread =
      make_unique_helper<std::thread>(listenThreadFunction, listen_address);

  std::cout << "Server started. Press Ctrl+C to stop." << std::endl;

  while (!stop_requested.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  if (listen_thread && listen_thread->joinable()) {
    listen_thread->join();
    listen_thread = nullptr;
  }

  std::cout << "Shutting down..." << std::endl;
  stopStreamingThread();
  std::cout << "All threads stopped. Exiting." << std::endl;
  return 0;
}
