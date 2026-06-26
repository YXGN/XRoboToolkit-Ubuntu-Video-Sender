/*
 * zed_webcam_common.cpp
 *
 * 实现 USB 摄像头 → BGRA →（可选预览分支）→ x264/x265 → appsink → TCP 封包。
 * --listen 与 --send 仅在「谁写入 current_camera_config / send_to_*」上不同，
 * 推流路径统一为本文件中的 streamingThreadFunction。
 *
 * 延迟优化（相对原始版本）：
 *   1. GStreamer queue 低延迟参数：
 *        queue max-size-buffers=1 max-size-time=0 max-size-bytes=0 leaky=downstream
 *      队列最多缓存 1 帧，满时丢弃最旧帧（leaky=downstream）。
 *      原默认 200 帧缓冲导致延迟随时间线性增大，此处已消除。
 *
 *   2. streaming loop 改用 waitNewFrame 条件变量：
 *      摄像头采集移至 WebcamCaptureSource 后台线程，主循环通过条件变量等待，
 *      每次保证处理最新帧，不再重复编码同一帧。
 *
 *   3. 全链路延迟统计（T1~T7）：每 30 帧打印一次各阶段延迟。
 */

#include "zed_webcam_common.hpp"
#include "webcam_capture_source.hpp"
#include "latency_tracker.hpp"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstring>
#include <cstdint>
#include <vector>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <iostream>
#include <opencv2/opencv.hpp>
#include <zmq.h>

/* ========== 全局状态（与 listen/send 共享）========== */

CameraRequestData current_camera_config;
std::atomic<uint64_t> current_camera_config_epoch{0};

std::atomic<bool> stop_requested{false};
std::atomic<bool> streaming_active{false};
std::atomic<bool> encoding_enabled{false};
std::atomic<bool> send_enabled{false};
std::atomic<bool> preview_enabled{false};
std::atomic<bool> zmq_enabled{false};
std::atomic<bool> zmq_raw_mode{false};

std::mutex config_mutex;
std::condition_variable streaming_cv;
std::mutex streaming_mutex;

std::unique_ptr<TCPClient> sender_ptr;
std::unique_ptr<asio_net::UDPClient> udp_sender_ptr;
std::string send_to_server;
int send_to_port = 0;
std::string send_protocol = "tcp";

std::string g_cli_camera_path;
std::string g_cli_stereo_camera_path;
std::string g_zmq_endpoint;

namespace {

/* 推流工作线程句柄；由 streaming_mutex 保护创建与 join */
std::unique_ptr<std::thread> streaming_thread;
std::atomic<uint64_t> transport_frame_sequence{0};
void *zmq_context = nullptr;
void *zmq_publisher = nullptr;
std::mutex zmq_mutex;

struct LoopFpsStats {
    const char *tag;
    int report_every;
    uint64_t count;
    std::chrono::steady_clock::time_point start_tp;

    explicit LoopFpsStats(const char *tag_name, int every = 120)
        : tag(tag_name),
          report_every(every),
          count(0),
          start_tp(std::chrono::steady_clock::now()) {}

    void tick(const std::string &detail = std::string()) {
        ++count;
        if (report_every <= 0 || (count % static_cast<uint64_t>(report_every)) != 0) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        const double elapsed =
            std::chrono::duration_cast<std::chrono::duration<double>>(now - start_tp).count();
        const double fps = elapsed > 0.0 ? static_cast<double>(report_every) / elapsed : 0.0;
        std::cout << "[" << tag << "] fps=" << fps;
        if (!detail.empty()) {
            std::cout << " " << detail;
        }
        std::cout << std::endl;
        start_tp = now;
    }
};

static constexpr uint16_t kEncodedTransportVersion = 2;
static constexpr uint16_t kEncodedTransportHeaderSize = 40;
static constexpr size_t kEncodedOuterHeaderSize = 4;
static constexpr size_t kEncodedInnerHeaderSize = 40;
static constexpr uint16_t kRawTransportVersion = 1;
static constexpr uint16_t kRawTransportHeaderSize = 56;
static constexpr uint32_t kRawPixelFormatBgra8 = 1;

static int64_t GetUnixTimeMicroseconds() {
    using namespace std::chrono;
    return duration_cast<microseconds>(
               system_clock::now().time_since_epoch())
        .count();
}

static void WriteLe16(uint8_t *dst, uint16_t value) {
    dst[0] = static_cast<uint8_t>(value & 0xFF);
    dst[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
}

static void WriteLe32(uint8_t *dst, uint32_t value) {
    dst[0] = static_cast<uint8_t>(value & 0xFF);
    dst[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    dst[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
    dst[3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

static void WriteLe64(uint8_t *dst, uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        dst[i] = static_cast<uint8_t>((value >> (i * 8)) & 0xFF);
    }
}

static void WriteBe32(uint8_t *dst, uint32_t value) {
    dst[0] = static_cast<uint8_t>((value >> 24) & 0xFF);
    dst[1] = static_cast<uint8_t>((value >> 16) & 0xFF);
    dst[2] = static_cast<uint8_t>((value >> 8) & 0xFF);
    dst[3] = static_cast<uint8_t>(value & 0xFF);
}

static void WriteBe64(uint8_t *dst, uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        const int shift = (7 - i) * 8;
        dst[i] = static_cast<uint8_t>((value >> shift) & 0xFF);
    }
}

static std::vector<uint8_t> BuildTransportPacket(const uint8_t *payload,
                                                 size_t payload_size,
                                                 uint64_t frame_id,
                                                 int64_t sender_capture_utc_us,
                                                 int64_t sender_send_utc_us) {
    const uint32_t body_size =
        static_cast<uint32_t>(kEncodedInnerHeaderSize + payload_size);
    std::vector<uint8_t> packet(kEncodedOuterHeaderSize + body_size);

    packet[0] = static_cast<uint8_t>((body_size >> 24) & 0xFF);
    packet[1] = static_cast<uint8_t>((body_size >> 16) & 0xFF);
    packet[2] = static_cast<uint8_t>((body_size >> 8) & 0xFF);
    packet[3] = static_cast<uint8_t>(body_size & 0xFF);

    uint8_t *header = packet.data() + kEncodedOuterHeaderSize;
    header[0] = 'X';
    header[1] = 'R';
    header[2] = 'L';
    header[3] = 'T';
    WriteLe16(header + 4, kEncodedTransportVersion);
    WriteLe16(header + 6, kEncodedTransportHeaderSize);
    WriteLe64(header + 8, frame_id);
    WriteLe64(header + 16, static_cast<uint64_t>(sender_capture_utc_us));
    WriteLe64(header + 24, static_cast<uint64_t>(sender_send_utc_us));
    WriteLe32(header + 32, static_cast<uint32_t>(payload_size));
    WriteLe32(header + 36, 0);

    if (payload_size > 0) {
        std::copy(payload, payload + payload_size,
                  packet.begin() + kEncodedOuterHeaderSize + kEncodedInnerHeaderSize);
    }

    return packet;
}

static std::vector<uint8_t> BuildRawImagePacket(const cv::Mat &bgra,
                                                uint64_t frame_id,
                                                int64_t sender_capture_utc_us,
                                                int64_t sender_send_utc_us) {
    cv::Mat contiguous = bgra.isContinuous() ? bgra : bgra.clone();
    const uint32_t width = static_cast<uint32_t>(contiguous.cols);
    const uint32_t height = static_cast<uint32_t>(contiguous.rows);
    const uint32_t channels = static_cast<uint32_t>(contiguous.channels());
    const uint32_t payload_size =
        static_cast<uint32_t>(contiguous.total() * contiguous.elemSize());

    /*
     * xr_teleoperate/teleop/utils/episode_writer.py 当前接收的 raw ZMQ 协议是:
     *   [4B magic='XRAW'][4B version=1]
     *   [4B width][4B height][4B channels]
     *   [8B frame_seq][8B source_wall_time_ns][8B source_monotonic_ns]
     *   [raw image bytes]
     *
     * 注意：
     * - 所有整数均为 big-endian
     * - source_* 时间戳单位为 ns
     * - 接收端支持 channels=3/4；4 通道会在 host 侧转回 BGR
     */
    constexpr uint32_t kXrawVersion = 1;
    constexpr size_t kXrawHeaderSize = 44;
    std::vector<uint8_t> packet(kXrawHeaderSize + payload_size);
    uint8_t *header = packet.data();
    header[0] = 'X';
    header[1] = 'R';
    header[2] = 'A';
    header[3] = 'W';
    WriteBe32(header + 4, kXrawVersion);
    WriteBe32(header + 8, width);
    WriteBe32(header + 12, height);
    WriteBe32(header + 16, channels);
    WriteBe64(header + 20, frame_id);
    WriteBe64(header + 28, static_cast<uint64_t>(sender_capture_utc_us) * 1000ULL);
    WriteBe64(header + 36, static_cast<uint64_t>(sender_send_utc_us) * 1000ULL);
    if (payload_size > 0) {
        std::memcpy(packet.data() + kXrawHeaderSize,
                    contiguous.data,
                    payload_size);
    }
    return packet;
}

static bool initialize_zmq() {
    if (g_zmq_endpoint.empty()) return false;

    std::lock_guard<std::mutex> lock(zmq_mutex);
    if (zmq_publisher) return true;

    zmq_context = zmq_ctx_new();
    if (!zmq_context) {
        std::cerr << "Failed to create ZMQ context" << std::endl;
        return false;
    }

    zmq_publisher = zmq_socket(zmq_context, ZMQ_PUB);
    if (!zmq_publisher) {
        std::cerr << "Failed to create ZMQ publisher" << std::endl;
        zmq_ctx_destroy(zmq_context);
        zmq_context = nullptr;
        return false;
    }

    const int hwm = 2;
    const int linger = 0;
    zmq_setsockopt(zmq_publisher, ZMQ_SNDHWM, &hwm, sizeof(hwm));
    zmq_setsockopt(zmq_publisher, ZMQ_LINGER, &linger, sizeof(linger));

    if (zmq_bind(zmq_publisher, g_zmq_endpoint.c_str()) != 0) {
        std::cerr << "Failed to bind ZMQ publisher to " << g_zmq_endpoint
                  << ": " << zmq_strerror(zmq_errno()) << std::endl;
        zmq_close(zmq_publisher);
        zmq_publisher = nullptr;
        zmq_ctx_destroy(zmq_context);
        zmq_context = nullptr;
        return false;
    }

    std::cout << "ZMQ publisher bound to " << g_zmq_endpoint
              << (zmq_raw_mode.load() ? " [raw]" : " [encoded]")
              << std::endl;
    return true;
}

static void publish_zmq_packet(const std::vector<uint8_t> &packet) {
    std::lock_guard<std::mutex> lock(zmq_mutex);
    if (!zmq_enabled.load() || !zmq_publisher || packet.empty()) return;

    int rc = zmq_send(zmq_publisher,
                      packet.data(),
                      packet.size(),
                      ZMQ_DONTWAIT);
    if (rc == -1 && zmq_errno() != EAGAIN) {
        std::cerr << "ZMQ send error: " << zmq_strerror(zmq_errno()) << std::endl;
    }
}

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
    /* T6：GStreamer 编码线程触发本回调的时刻 */
    const int64_t t6 = lat_now_ns();
    /* T5：从原子变量读取上一次 appsrc push 完成的时刻，用于计算编码耗时 */
    const int64_t t5 = LatencyTracker::get().last_push_ns.load();

    GstSample *sample = gst_app_sink_pull_sample(sink);
    if (!sample)
        return GST_FLOW_ERROR;

    GstBuffer *buffer = gst_sample_get_buffer(sample);
    GstMapInfo map;
    if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        const uint8_t *data = map.data;
        gsize size = map.size;
        const bool tcp_ready =
            send_enabled.load() && sender_ptr && sender_ptr->isConnected() &&
            data && size > 0;

        const bool udp_ready =
            send_enabled.load() && udp_sender_ptr && udp_sender_ptr->isConnected() &&
            data && size > 0;

        const bool zmq_ready =
            !zmq_raw_mode.load() && zmq_enabled.load() && data && size > 0;
        uint64_t frame_id = 0;
        int64_t sender_capture_utc_us = 0;
        int64_t sender_send_utc_us = 0;
        std::vector<uint8_t> packet;
        if (tcp_ready || zmq_ready) {
            frame_id = ++transport_frame_sequence;
            sender_capture_utc_us = GetUnixTimeMicroseconds();
            if (GST_BUFFER_PTS_IS_VALID(buffer) && GST_BUFFER_PTS(buffer) > 0) {
                sender_capture_utc_us =
                    static_cast<int64_t>(GST_BUFFER_PTS(buffer) / 1000);
            } else if (GST_BUFFER_DTS_IS_VALID(buffer) &&
                       GST_BUFFER_DTS(buffer) > 0) {
                sender_capture_utc_us =
                    static_cast<int64_t>(GST_BUFFER_DTS(buffer) / 1000);
            }
            sender_send_utc_us = GetUnixTimeMicroseconds();
            packet = BuildTransportPacket(data,
                                          static_cast<size_t>(size),
                                          frame_id,
                                          sender_capture_utc_us,
                                          sender_send_utc_us);
        }

        if (tcp_ready) {
            try {
                sender_ptr->sendData(packet);

                /* T7：TCP 发送完成（含阻塞等待） */
                const int64_t t7 = lat_now_ns();
                if (t5 > 0) {
                    LatencyTracker::get().add_encode_send(t5, t6, t7);
                }
            } catch (const TCPException &e) {
                std::cerr << "TCP error in on_new_sample: " << e.what() << std::endl;
                streaming_active.store(false);
            } catch (const std::exception &e) {
                std::cerr << "Unexpected error in on_new_sample: " << e.what() << std::endl;
                streaming_active.store(false);
            }
        } else if (udp_ready) {
            try {
                if (size <= kUdpMaxPayload) {
                    auto pkt = std::make_shared<std::vector<uint8_t>>(4 + size);
                    (*pkt)[0] = static_cast<uint8_t>((size >> 24) & 0xFF);
                    (*pkt)[1] = static_cast<uint8_t>((size >> 16) & 0xFF);
                    (*pkt)[2] = static_cast<uint8_t>((size >>  8) & 0xFF);
                    (*pkt)[3] = static_cast<uint8_t>(size & 0xFF);
                    std::memcpy(pkt->data() + 4, data, size);
                    udp_sender_ptr->sendDataAsync(*pkt);
                } else {
                    const size_t payload_per_frag = kUdpMaxPayload;
                    size_t offset = 0;
                    while (offset < size) {
                        size_t chunk = std::min(payload_per_frag, size - offset);
                        bool is_last = (offset + chunk >= size);
                        auto frag = std::make_shared<std::vector<uint8_t>>(10 + chunk);
                        (*frag)[0] = 0xFF;
                        (*frag)[1] = is_last ? 0xFF : 0x00;
                        (*frag)[2] = static_cast<uint8_t>((size >> 24) & 0xFF);
                        (*frag)[3] = static_cast<uint8_t>((size >> 16) & 0xFF);
                        (*frag)[4] = static_cast<uint8_t>((size >>  8) & 0xFF);
                        (*frag)[5] = static_cast<uint8_t>(size & 0xFF);
                        (*frag)[6] = static_cast<uint8_t>((offset >> 24) & 0xFF);
                        (*frag)[7] = static_cast<uint8_t>((offset >> 16) & 0xFF);
                        (*frag)[8] = static_cast<uint8_t>((offset >>  8) & 0xFF);
                        (*frag)[9] = static_cast<uint8_t>(offset & 0xFF);
                        std::memcpy(frag->data() + 10, data + offset, chunk);
                        udp_sender_ptr->sendDataAsync(*frag);
                        offset += chunk;
                    }
                }

                const int64_t t7 = lat_now_ns();
                if (t5 > 0) {
                    LatencyTracker::get().add_encode_send(t5, t6, t7);
                }
            } catch (const std::exception &e) {
                std::cerr << "UDP error in on_new_sample: " << e.what() << std::endl;
                streaming_active.store(false);
            }
        }

        if (zmq_ready) {
            try {
                publish_zmq_packet(packet);
            } catch (const std::exception &e) {
                std::cerr << "Unexpected error in ZMQ encoded publish: "
                          << e.what() << std::endl;
            }
        }

        gst_buffer_unmap(buffer, &map);
    }

    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

/* x264enc.bitrate 单位为 kbps；Open/Pico 侧 bitrate 常为 bps */
static int bitrateBpsToKbps(int bitrate_bps) {
    if (bitrate_bps <= 0) return 20000;
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
    int fps          = config.fps     > 0 ? config.fps     : 30;
    int w            = config.width   > 0 ? config.width   : 2560;
    int h            = config.height  > 0 ? config.height  : 720;
    int bitrate_kbps = bitrateBpsToKbps(config.bitrate);

    /*
     * GOP 设为 fps（约 1 秒），确保快速运动时画面能在 1 秒内完全刷新，
     * 消除 intra-refresh 模式下大幅运动产生的局部拖影问题。
     * vbv-buf-capacity 限制编码器 VBV 缓冲为 500 ms，强制近似 CBR，
     * 防止编码器在 I 帧附近爆出大数据包堵塞 TCP 回调线程。
     */
    int key_int_max = std::max(10, fps / 2);

    std::string pipeline_str =
        "appsrc name=mysource is-live=true format=time "
        "caps=video/x-raw,format=BGRA,width=" +
        std::to_string(w) + ",height=" + std::to_string(h) + ",framerate=" +
        std::to_string(fps) + "/1 ! ";

    pipeline_str += "videoconvert ! tee name=t ";

    /*
     * 低延迟队列参数（各支路统一）：
     *   max-size-buffers=1   队列最多缓存 1 帧
     *   max-size-time=0      不按时间限制
     *   max-size-bytes=0     不按字节限制
     *   leaky=downstream     队列满时丢弃最旧帧，保留最新帧
     *
     * 效果：appsrc 快于编码器时，编码器始终拿到最新帧，不发生帧堆积。
     * 代价：丢弃的帧不会被编码，接收端帧率可能低于相机帧率，但延迟恒定。
     */
    static const char *kLowLatQueue =
        "queue max-size-buffers=1 max-size-time=0 max-size-bytes=0 "
        "leaky=downstream ";

    if (config.enableMvHevc) {
        pipeline_str += std::string("t. ! ") + kLowLatQueue +
                        "! x265enc tune=zerolatency bitrate=" +
                        std::to_string(bitrate_kbps) +
                        " speed-preset=superfast key-int-max=" +
                        std::to_string(key_int_max) +
                        " ! h265parse ! appsink name=mysink emit-signals=true "
                        "sync=false ";
    } else {
        /*
         * 低延迟优化（UDP 场景）：
         *   - 无 vbv-buf-capacity   → 不限制 VBV，允许编码器即时输出
         *   - 无 nal-hrd=cbr        → 避免 HRD 强制延迟
         *   - tune=zerolatency      → 消除编码器内部参考帧延迟
         *   - bframes=0             → 无 B 帧，P/B 帧延迟最小
         *   - key-int-max=6         → 每 6 帧一个 I 帧（≈200ms），平衡延迟与压缩率
         *   - leaky=downstream      → 队列满时丢弃旧帧，保证实时性
         */
        pipeline_str += std::string("t. ! ") + kLowLatQueue +
            "! videoconvert ! video/x-raw,format=I420 ! "
            "x264enc profile=high tune=zerolatency bitrate=" +
            std::to_string(bitrate_kbps) +
            " speed-preset=superfast key-int-max=6 bframes=0 "
            "! h264parse "
            "! video/x-h264,stream-format=(string)byte-stream,"
            "alignment=(string)au "
            "! appsink name=mysink emit-signals=true sync=false ";
    }

    if (preview) {
        pipeline_str += std::string("t. ! ") + kLowLatQueue +
            "! videoconvert ! autovideosink sync=false ";
    }

    return pipeline_str;
}

struct EncodedPipelineState {
    GstElement *pipeline = nullptr;
    GstElement *appsrc = nullptr;
    GstElement *appsink = nullptr;
    CameraRequestData config;
    uint64_t applied_config_epoch = 0;
    int out_w = 2560;
    int out_h = 720;
    int fps = 30;
};

static CameraRequestData SnapshotCurrentCameraConfig(uint64_t *epoch_out = nullptr) {
    std::lock_guard<std::mutex> lock(config_mutex);
    if (epoch_out) {
        *epoch_out = current_camera_config_epoch.load();
    }
    return current_camera_config;
}

static bool StartEncodedPipeline(const CameraRequestData &config,
                                 uint64_t config_epoch,
                                 EncodedPipelineState &state) {
    std::string pipeline_str =
        buildWebcamPipelineString(config, preview_enabled.load());
    std::cout << "Webcam pipeline: " << pipeline_str << std::endl;

    GError *error = nullptr;
    GstElement *pipeline = gst_parse_launch(pipeline_str.c_str(), &error);
    if (!pipeline) {
        std::cerr << "Failed to create pipeline: "
                  << (error ? error->message : "?") << std::endl;
        g_clear_error(&error);
        return false;
    }

    GstElement *appsrc = gst_bin_get_by_name(GST_BIN(pipeline), "mysource");
    GstElement *appsink = gst_bin_get_by_name(GST_BIN(pipeline), "mysink");
    if (!appsrc || !appsink) {
        std::cerr << "Failed to get appsrc/appsink" << std::endl;
        if (appsrc) gst_object_unref(appsrc);
        if (appsink) gst_object_unref(appsink);
        gst_object_unref(pipeline);
        return false;
    }

    g_signal_connect(appsink, "new-sample", G_CALLBACK(on_new_sample), nullptr);

    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    state.pipeline = pipeline;
    state.appsrc = appsrc;
    state.appsink = appsink;
    state.config = config;
    state.applied_config_epoch = config_epoch;
    state.out_w = config.width > 0 ? config.width : 2560;
    state.out_h = config.height > 0 ? config.height : 720;
    state.fps = config.fps > 0 ? config.fps : 30;
    std::cout << "[encoded] pipeline started @ " << state.out_w << "x"
              << state.out_h << " " << state.fps << "fps epoch="
              << state.applied_config_epoch << std::endl;
    return true;
}

static void StopEncodedPipeline(EncodedPipelineState &state) {
    if (!state.pipeline) {
        state.appsrc = nullptr;
        state.appsink = nullptr;
        state.applied_config_epoch = 0;
        return;
    }

    gst_app_src_end_of_stream(GST_APP_SRC(state.appsrc));
    gst_element_set_state(state.pipeline, GST_STATE_NULL);
    if (state.appsrc) gst_object_unref(state.appsrc);
    if (state.appsink) gst_object_unref(state.appsink);
    gst_object_unref(state.pipeline);

    state.pipeline = nullptr;
    state.appsrc = nullptr;
    state.appsink = nullptr;
    state.applied_config_epoch = 0;
}

} // namespace

void zed_webcam_install_sigint_handler() {
    signal(SIGINT, zed_webcam_on_sigint);
}

void zed_webcam_set_preview_enabled(bool v) { preview_enabled.store(v); }

void zed_webcam_set_camera_paths(const std::string &mono,
                                  const std::string &stereo) {
    g_cli_camera_path        = mono;
    g_cli_stereo_camera_path = stereo;
}

void zed_webcam_set_zmq_endpoint(const std::string &endpoint, bool raw_mode) {
    g_zmq_endpoint = endpoint;
    zmq_raw_mode.store(raw_mode);
}

bool zed_webcam_has_zmq_endpoint() {
    return !g_zmq_endpoint.empty();
}

void zed_webcam_cleanup_zmq() {
    std::lock_guard<std::mutex> lock(zmq_mutex);
    zmq_enabled.store(false);
    if (zmq_publisher) {
        zmq_close(zmq_publisher);
        zmq_publisher = nullptr;
    }
    if (zmq_context) {
        zmq_ctx_destroy(zmq_context);
        zmq_context = nullptr;
    }
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

bool initialize_udp_sender() {
    if (send_to_server.empty() || send_to_port <= 0) return false;
    try {
        udp_sender_ptr = make_unique_helper<asio_net::UDPClient>(send_to_server, send_to_port);
        udp_sender_ptr->connect();
        return true;
    } catch (const std::exception &e) {
        std::cerr << "Failed to initialize UDP sender: " << e.what() << std::endl;
        udp_sender_ptr = nullptr;
        return false;
    }
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

    if (udp_sender_ptr) {
        udp_sender_ptr->disconnect();
        udp_sender_ptr = nullptr;
    }

    if (streaming_thread && streaming_thread->joinable()) {
        streaming_cv.notify_all();
        streaming_thread->join();
        streaming_thread = nullptr;
        std::cout << "Stopped streaming thread" << std::endl;
    }
}

void stopTcpSending() {
    send_enabled.store(false);
    if (sender_ptr && sender_ptr->isConnected()) {
        try {
            sender_ptr->disconnect();
        } catch (const std::exception &e) {
            std::cerr << "TCP disconnect warning: " << e.what() << std::endl;
        }
    }
    sender_ptr = nullptr;

    if (udp_sender_ptr) {
        udp_sender_ptr->disconnect();
        udp_sender_ptr = nullptr;
    }
}

/*
 * 推流线程（与主线程、GStreamer 回调线程并发）：
 *  1) TCP 连接接收端
 *  2) 拷贝一份 current_camera_config，确定输出宽高 fps
 *  3) 选择设备路径（CLI 或自动 1080p 探测）
 *  4) WebcamCaptureSource::open() → 后台采集线程启动
 *  5) GStreamer parse_launch + PLAYING
 *  6) 循环：waitNewFrame → readFrame → cvtColor → resize → appsrc push
 *
 * h264parse 配置为 AU 对齐，故 appsink 单次回调对应一块可独立解码的负载。
 */
void streamingThreadFunction() {
    std::cout << "Streaming thread started" << std::endl;

    try {
        bool tcp_initialized = false;
        bool udp_initialized = false;

        if (!send_to_server.empty() && send_to_port > 0) {
            if (send_protocol == "udp") {
                udp_initialized = initialize_udp_sender();
                if (!udp_initialized && !zed_webcam_has_zmq_endpoint()) {
                    std::cerr << "UDP init failed, streaming thread stopping" << std::endl;
                    return;
                }
            } else {
                tcp_initialized = initialize_sender();
                if (!tcp_initialized) {
                    std::cerr << "Failed to initialize TCP sender";
                    if (!zed_webcam_has_zmq_endpoint()) {
                        std::cerr << ", streaming thread stopping" << std::endl;
                        return;
                    }
                    std::cerr << ", continue with ZMQ only" << std::endl;
                }
            }
        }

        const bool zmq_initialized =
            zed_webcam_has_zmq_endpoint() && initialize_zmq();
        if (zed_webcam_has_zmq_endpoint()) {
            zmq_enabled.store(zmq_initialized);
        }

        if (!tcp_initialized && !udp_initialized && !zmq_initialized) {
            std::cerr << "No output available (TCP/UDP/ZMQ all unavailable), "
                         "streaming thread stopping"
                      << std::endl;
            return;
        }

        encoding_enabled.store(true);
        send_enabled.store(tcp_initialized || udp_initialized);

        CameraRequestData config = SnapshotCurrentCameraConfig(nullptr);
        int capture_fps = config.fps > 0 ? config.fps : 30;

        bool use_stereo_sbs = !g_cli_stereo_camera_path.empty();
        std::string device_path = use_stereo_sbs ? g_cli_stereo_camera_path
                                                 : g_cli_camera_path;
        /* 空串时 WebcamCaptureSource::open() 内部自动探测 */

        WebcamCaptureSource webcam;
        std::string open_err;
        if (!webcam.open(device_path, use_stereo_sbs, capture_fps, open_err)) {
            std::cerr << "无法打开摄像头";
            if (!open_err.empty()) std::cerr << ": " << open_err;
            std::cerr << std::endl;
            return;
        }

        int      frame_id  = 0;
        uint64_t frame_seq = 0; /* 上一次处理的帧序号，配合 waitNewFrame 避免重复处理 */
        EncodedPipelineState encoded_pipeline;
        LoopFpsStats stream_fps("stream");
        LoopFpsStats raw_pub_fps("raw_pub");
        static bool printed_raw_mode_notice = false;

        std::cout << "Starting webcam streaming loop..." << std::endl;
        while (streaming_active.load() && !stop_requested.load()) {
            /*
             * 等待后台采集线程推送新帧（最多 200ms）。
             * 超时则重新检查 stop 标志，不处理帧。
             * 这保证了 loop 速率精确对齐相机帧率，不会重复编码同一帧。
             */
            if (!webcam.waitNewFrame(frame_seq, 200)) {
                if (!webcam.isOpen()) {
                    std::cerr << "后台采集线程已退出，停止推流" << std::endl;
                    break;
                }
                continue; /* 超时：相机帧率低或短暂掉帧，继续等 */
            }

            /* 读帧，收集 T1（采集时刻）和 T2（主线程拿到帧的时刻） */
            int64_t t1 = 0, t2 = 0;
            cv::Mat frame;
            if (!webcam.readFrame(frame, &t1, &t2) || frame.empty()) {
                std::cerr << "readFrame failed" << std::endl;
                break;
            }
            const int64_t sender_capture_utc_us = GetUnixTimeMicroseconds();
            const bool need_encoded_output =
                send_enabled.load() || (!zmq_raw_mode.load() && zmq_enabled.load()) ||
                preview_enabled.load();
            const bool raw_input_looks_side_by_side = (frame.cols >= frame.rows * 2);

            cv::Mat raw_publish_frame;
            if (use_stereo_sbs || raw_input_looks_side_by_side) {
                /*
                 * 数采 raw 模式下，不再保留 side-by-side 双目拼接，
                 * 直接取左目作为这一机位的 RGB。
                 * 假设输入是左右并排：| LEFT | RIGHT |
                 */
                if (!use_stereo_sbs && raw_input_looks_side_by_side) {
                    static bool printed_raw_sbs_notice = false;
                    if (!printed_raw_sbs_notice) {
                        std::cout << "[raw] detected side-by-side input " << frame.cols
                                  << "x" << frame.rows
                                  << ", publishing LEFT half only for collection."
                                  << std::endl;
                        printed_raw_sbs_notice = true;
                    }
                }
                if (frame.cols >= 2) {
                    const int left_width = frame.cols / 2;
                    raw_publish_frame = frame(cv::Rect(0, 0, left_width, frame.rows));
                } else {
                    raw_publish_frame = frame;
                }
            } else {
                /* 单目设备 raw 数采时直接保留原图，不再复制成双路。 */
                raw_publish_frame = frame;
            }

            /*
             * raw 数采分支始终保留自己的分辨率，不跟随 Pico/3D viewer 的
             * OPEN_CAMERA 输出分辨率变化，避免 host 侧看到“相机分辨率被改了”。
             */
            const cv::Mat &raw_out_frame = raw_publish_frame;

            if (zmq_raw_mode.load() && zmq_enabled.load()) {
                try {
                    const uint64_t frame_id = ++transport_frame_sequence;
                    const int64_t sender_send_utc_us = GetUnixTimeMicroseconds();
                    if (!printed_raw_mode_notice) {
                        std::cout << "[raw] publish format channels="
                                  << raw_out_frame.channels()
                                  << " size=" << raw_out_frame.cols << "x"
                                  << raw_out_frame.rows << std::endl;
                        printed_raw_mode_notice = true;
                    }
                    publish_zmq_packet(BuildRawImagePacket(raw_out_frame,
                                                           frame_id,
                                                           sender_capture_utc_us,
                                                           sender_send_utc_us));
                    raw_pub_fps.tick("channels=" + std::to_string(raw_out_frame.channels()) +
                                     " size=" + std::to_string(raw_out_frame.cols) + "x" +
                                     std::to_string(raw_out_frame.rows));
                } catch (const std::exception &e) {
                    std::cerr << "Unexpected error in ZMQ raw publish: "
                              << e.what() << std::endl;
                }
            }

            if (!encoding_enabled.load()) break;
            if (!need_encoded_output) {
                stream_fps.tick("raw_only=1");
                if (encoded_pipeline.pipeline != nullptr) {
                    std::cout << "[encoded] no encoded consumer, stopping encoded pipeline only" << std::endl;
                    StopEncodedPipeline(encoded_pipeline);
                }
                continue;
            }

            /* T3：BGR→BGRA 色彩转换完成 */
            cv::Mat bgra;
            if (frame.channels() == 4) {
                bgra = frame;
            } else {
                cv::cvtColor(frame, bgra, cv::COLOR_BGR2BGRA);
            }
            const int64_t t3 = lat_now_ns();

            const uint64_t desired_epoch = current_camera_config_epoch.load();
            if (encoded_pipeline.pipeline == nullptr ||
                encoded_pipeline.applied_config_epoch != desired_epoch) {
                if (encoded_pipeline.pipeline != nullptr) {
                    std::cout << "[encoded] reconfiguring pipeline from epoch="
                              << encoded_pipeline.applied_config_epoch
                              << " to epoch=" << desired_epoch << std::endl;
                    StopEncodedPipeline(encoded_pipeline);
                }
                CameraRequestData desired_config =
                    SnapshotCurrentCameraConfig(nullptr);
                if (!StartEncodedPipeline(desired_config, desired_epoch, encoded_pipeline)) {
                    std::cerr << "[encoded] failed to (re)start pipeline, keep raw/ZMQ alive"
                              << std::endl;
                    continue;
                }
            }

            cv::Mat side_by_side;
            if (use_stereo_sbs) {
                /* 设备已输出左右并排，仅缩放至 Pico 请求分辨率 */
                side_by_side = bgra;
                static std::atomic<int64_t> last_warn_us{0};
                int64_t now_us = GetUnixTimeMicroseconds();
                if (now_us - last_warn_us.load() > 5000000) { /* 每 5 秒最多一次 */
                    std::cout << "[warn] stereo-sbs 模式下输入宽高比偏小: "
                              << side_by_side.cols << "x" << side_by_side.rows
                              << "，请确认设备输出是否为左右拼接" << std::endl;
                    last_warn_us.store(now_us);
                }
            } else {
                /* 单目模拟 ZED SIDE_BY_SIDE：复制左右两半 */
                cv::hconcat(bgra, bgra, side_by_side);
            }

            cv::Mat out_bgra;
            cv::resize(side_by_side, out_bgra,
                       cv::Size(encoded_pipeline.out_w, encoded_pipeline.out_h), 0, 0,
                       cv::INTER_LINEAR);

            /* T4：encoded resize 完成，out_bgra 就绪 */
            const int64_t t4 = lat_now_ns();

            size_t frame_bytes =
                static_cast<size_t>(out_bgra.total() * out_bgra.elemSize());
            GstBuffer *buffer =
                gst_buffer_new_allocate(nullptr, frame_bytes, nullptr);
            GstMapInfo map;
            if (!gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
                gst_buffer_unref(buffer);
                break;
            }
            std::memcpy(map.data, out_bgra.data, frame_bytes);
            gst_buffer_unmap(buffer, &map);

            /* 运行时钟：按 fps 均匀递增，与 live appsrc 协商一致 */
            GST_BUFFER_PTS(buffer) =
                static_cast<GstClockTime>(sender_capture_utc_us * 1000);
            GST_BUFFER_DTS(buffer) = GST_BUFFER_PTS(buffer);
            GST_BUFFER_DURATION(buffer) =
                gst_util_uint64_scale(1, GST_SECOND, encoded_pipeline.fps);
            GST_BUFFER_OFFSET(buffer) = static_cast<guint64>(frame_id);

            GstFlowReturn ret =
                gst_app_src_push_buffer(GST_APP_SRC(encoded_pipeline.appsrc), buffer);

            /* T5：appsrc push 返回（若 queue 满/阻塞，此处耗时会显著增大） */
            const int64_t t5 = lat_now_ns();

            /* 向 on_new_sample 线程传递 T5，供计算编码耗时（T6-T5） */
            LatencyTracker::get().last_push_ns.store(t5);

            /* 记录采集侧各阶段延迟（T1 有效则上报） */
            if (t1 > 0) {
                LatencyTracker::get().add_capture(t1, t2, t3, t4, t5);
            }

            if (ret != GST_FLOW_OK) {
                std::cerr << "appsrc push failed: " << ret << std::endl;
                break;
            }

            stream_fps.tick("raw_only=0");
            frame_id++;
        }

        std::cout << "Streaming loop ended, cleaning up..." << std::endl;

        StopEncodedPipeline(encoded_pipeline);
        webcam.close();

    } catch (const std::exception &e) {
        std::cerr << "Streaming thread error: " << e.what() << std::endl;
    }

    std::cout << "Streaming thread finished" << std::endl;
}
