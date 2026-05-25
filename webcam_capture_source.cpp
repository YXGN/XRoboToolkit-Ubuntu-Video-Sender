/*
 * webcam_capture_source.cpp
 *
 * WebcamCaptureSource 实现：后台线程 cap.read() + 条件变量 waitNewFrame()。
 * 同时包含摄像头设备探测与打开的辅助函数（原属 zed_webcam_common.cpp）。
 */

#include "webcam_capture_source.hpp"
#include "latency_tracker.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <glob.h>
#include <iostream>
#include <unistd.h>
#include <vector>

#include <opencv2/imgproc.hpp>

/* ============================================================
 * 设备探测辅助（file-local）
 * ============================================================ */

namespace {

/* 双目 MJPEG SBS 标准采集分辨率（与 v4l2 枚举一致） */
static constexpr int kStereoSbsCaptureWidth  = 1856;
static constexpr int kStereoSbsCaptureHeight = 800;

/*
 * 探测某 /dev/video 节点能否以 1920×1080 实际采到一帧。
 * 用于单目自动选型时过滤元数据节点（/dev/video1 等）。
 */
static bool probe1080pDevice(const std::string &device_path, std::string &err) {
    cv::VideoCapture cap(device_path);
    if (!cap.isOpened()) {
        err = "无法打开";
        return false;
    }
    cap.set(cv::CAP_PROP_FRAME_WIDTH,  1920);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, 1080);
    cv::Mat frame;
    if (!cap.read(frame) || frame.empty()) {
        err = "read 失败";
        cap.release();
        return false;
    }
    if (frame.cols != 1920 || frame.rows != 1080) {
        err = "实际分辨率 " + std::to_string(frame.cols) + "x" +
              std::to_string(frame.rows);
        cap.release();
        return false;
    }
    int w = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_WIDTH));
    int h = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_HEIGHT));
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
    std::memset(&gbuf, 0, sizeof(gbuf));
    if (::glob("/dev/video*", 0, nullptr, &gbuf) != 0) {
        std::cerr << "glob /dev/video* 失败: " << std::strerror(errno) << std::endl;
        return {};
    }

    std::vector<std::pair<int, std::string>> numbered;
    for (size_t i = 0; i < gbuf.gl_pathc; ++i) {
        std::string p = gbuf.gl_pathv[i];
        int n = -1;
        if (std::sscanf(p.c_str(), "/dev/video%d", &n) != 1) continue;
        numbered.emplace_back(n, p);
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
    return {};
}

/*
 * 打开 V4L2 摄像头（优先 CAP_V4L2，避免与其他 GStreamer 进程争用默认后端）。
 *   stereo_sbs : MJPEG + 固定宽高；否则单目 1920×1080
 *   fps        : 与 GStreamer 管线一致，用于驱动侧协商
 */
static bool openUsbCapture(const std::string &device, bool stereo_sbs, int fps,
                            cv::VideoCapture &cap, std::string &err_detail) {
    err_detail.clear();

    auto configure = [&](cv::VideoCapture &c) -> bool {
        if (!c.isOpened()) return false;
        if (stereo_sbs) {
            c.set(cv::CAP_PROP_FOURCC,
                  cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
            c.set(cv::CAP_PROP_FRAME_WIDTH,  kStereoSbsCaptureWidth);
            c.set(cv::CAP_PROP_FRAME_HEIGHT, kStereoSbsCaptureHeight);
        } else {
            c.set(cv::CAP_PROP_FRAME_WIDTH,  1920);
            c.set(cv::CAP_PROP_FRAME_HEIGHT, 1080);
        }
        c.set(cv::CAP_PROP_FPS, fps);
        return true;
    };

    cv::VideoCapture tmp;

    int vid = -1;
    if (std::sscanf(device.c_str(), "/dev/video%d", &vid) == 1 && vid >= 0) {
        tmp.open(vid, cv::CAP_V4L2);
        if (configure(tmp)) { cap = std::move(tmp); return true; }
        tmp.release();
    }

    tmp.open(device, cv::CAP_V4L2);
    if (configure(tmp)) { cap = std::move(tmp); return true; }
    tmp.release();

    tmp.open(device);
    if (configure(tmp)) { cap = std::move(tmp); return true; }
    tmp.release();

    int fd = ::open(device.c_str(), O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        err_detail = std::strerror(errno);
    } else {
        ::close(fd);
        err_detail = "OpenCV 仍未打开（底层节点可读）；若需独占请先关掉其它取流进程";
    }
    return false;
}

} // namespace

/* ============================================================
 * WebcamCaptureSource
 * ============================================================ */

WebcamCaptureSource::WebcamCaptureSource() = default;

WebcamCaptureSource::~WebcamCaptureSource() {
    close();
}

bool WebcamCaptureSource::open(const std::string &device_path,
                                bool stereo_sbs, int fps,
                                std::string &err_detail) {
    close();

    /* 解析设备路径（空串 + 非 stereo 则自动探测） */
    std::string device;
    if (stereo_sbs) {
        device = device_path;
        if (device.empty()) {
            err_detail = "stereo-sbs 模式需通过 --stereo-camera 指定设备路径";
            return false;
        }
    } else {
        device = device_path.empty() ? pickAuto1080pDevice() : device_path;
        if (device.empty()) {
            err_detail = "未找到可用摄像头（auto 1080p 探测失败），"
                         "请使用 --camera /dev/videoN 手动指定";
            return false;
        }
    }

    std::cout << "采集模式: " << (stereo_sbs ? "stereo-sbs" : "mono-copy")
              << "，设备: " << device << std::endl;

    if (!openUsbCapture(device, stereo_sbs, fps, cap_, err_detail)) {
        std::cerr << "无法打开摄像头: " << device;
        if (!err_detail.empty()) std::cerr << " — " << err_detail;
        std::cerr << std::endl;
        return false;
    }

    /* 启动后台采集线程 */
    running_.store(true);
    capture_thread_ = std::thread(&WebcamCaptureSource::captureLoop, this);

    /* 等待首帧（最多 3 s） */
    for (int i = 0; i < 60; ++i) {
        {
            std::lock_guard<std::mutex> lk(frame_mutex_);
            if (has_frame_) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    {
        std::lock_guard<std::mutex> lk(frame_mutex_);
        if (!has_frame_) {
            err_detail = "等待首帧超时（3s）";
            close();
            return false;
        }
    }

    std::cout << "[WebcamCaptureSource] 后台采集线程已启动，设备: " << device << std::endl;
    return true;
}

void WebcamCaptureSource::captureLoop() {
    cv::Mat frame;
    while (running_.load()) {
        if (!cap_.read(frame) || frame.empty()) {
            std::cerr << "[WebcamCaptureSource] cap.read() 失败，后台采集线程退出" << std::endl;
            running_.store(false);
            frame_cv_.notify_all(); /* 唤醒可能正在等待的主线程，让其检测 isOpen() */
            break;
        }

        /*
         * T1：帧从 V4L2 驱动出来的时刻。
         * 在持锁前记录，避免被锁等待时间污染（与 UvcCameraSource::frameCallback 一致）。
         */
        const int64_t t1 = lat_now_ns();

        {
            std::lock_guard<std::mutex> lk(frame_mutex_);
            /*
             * 直接覆盖最新帧（leaky 语义）：
             * 若主线程尚未消费上一帧，旧帧被新帧替换，不发生积压。
             * clone() 保证 cap_ 内部复用缓冲后本帧数据不被覆盖。
             */
            latest_frame_         = frame.clone();
            latest_frame_time_ns_ = t1;
            has_frame_            = true;
            frame_seq_++;
        }
        frame_cv_.notify_one();
    }
}

bool WebcamCaptureSource::waitNewFrame(uint64_t &last_seq, int timeout_ms) {
    std::unique_lock<std::mutex> lk(frame_mutex_);
    bool arrived = frame_cv_.wait_for(
        lk,
        std::chrono::milliseconds(timeout_ms),
        [this, &last_seq] { return frame_seq_ != last_seq; });
    if (arrived)
        last_seq = frame_seq_;
    return arrived;
}

bool WebcamCaptureSource::readFrame(cv::Mat &bgr,
                                     int64_t *t1_ns,
                                     int64_t *t2_ns) {
    {
        std::lock_guard<std::mutex> lk(frame_mutex_);
        if (!has_frame_ || latest_frame_.empty()) return false;
        if (t1_ns) *t1_ns = latest_frame_time_ns_;
        bgr = latest_frame_.clone();
        /* 标记已消费：防止调用方在无 waitNewFrame 保护时重复读同一帧 */
        has_frame_ = false;
    }
    /* T2：帧 clone 完成，主线程首次持有该帧 */
    if (t2_ns) *t2_ns = lat_now_ns();
    return !bgr.empty();
}

void WebcamCaptureSource::close() {
    running_.store(false);
    frame_cv_.notify_all();

    if (capture_thread_.joinable()) {
        capture_thread_.join();
    }
    cap_.release();

    std::lock_guard<std::mutex> lk(frame_mutex_);
    latest_frame_.release();
    has_frame_  = false;
    frame_seq_  = 0;
}
