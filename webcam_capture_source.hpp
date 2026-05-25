#pragma once

/*
 * webcam_capture_source.hpp
 *
 * 将 cv::VideoCapture::read() 移入独立后台线程，通过条件变量向 streaming loop
 * 通知新帧到达，解决以下两个延迟问题：
 *
 * 问题 1 — streaming loop 无限速 / 重复编码：
 *   原实现在主循环中直接调用 cap.read()（阻塞）。OpenCV V4L2 后端内部维护
 *   一个多帧环形缓冲队列；当编码速度慢于相机帧率时，驱动侧帧持续积压，
 *   cap.read() 虽然"立即"返回，实际返回的是队列中最早的旧帧，导致延迟
 *   随时间线性增大（同一帧被编码约 2.3 次的现象）。
 *
 * 解决方案：
 *   后台线程以最快速度持续调用 cap.read()，始终将最新帧写入共享缓冲，旧帧
 *   直接被覆盖（类似 leaky 语义）。主线程通过 waitNewFrame() 条件变量等待，
 *   每次都拿到最新帧，loop 速率自然对齐相机帧率，不再重复编码同一帧。
 *
 * 时间戳语义（配合 latency_tracker.hpp）：
 *   T1  后台线程 cap.read() 返回后立刻记录（帧从 V4L2 驱动出来的时刻）
 *   T2  主线程 readFrame() 完成帧 clone 后记录（主线程首次持有该帧的时刻）
 */

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>

class WebcamCaptureSource {
public:
    WebcamCaptureSource();
    ~WebcamCaptureSource();

    WebcamCaptureSource(const WebcamCaptureSource &) = delete;
    WebcamCaptureSource &operator=(const WebcamCaptureSource &) = delete;

    /*
     * 打开摄像头并启动后台采集线程。
     *   device_path : V4L2 设备路径（如 "/dev/video0"）；
     *                 空串 + !stereo_sbs 时触发自动探测（遍历 /dev/video*）
     *   stereo_sbs  : true 则请求 MJPEG 双目 SBS 采集分辨率（1856×800）
     *   fps         : 目标帧率，传给 V4L2 驱动协商
     *   err_detail  : 失败时填入可读错误描述
     */
    bool open(const std::string &device_path, bool stereo_sbs, int fps,
              std::string &err_detail);

    /*
     * 阻塞等待下一帧（与 last_seq 配合，确保每帧只被处理一次）。
     *   last_seq   : 调用前持有上次返回的序号（首次传 0）；
     *                返回 true 时自动更新为最新序号。
     *   timeout_ms : 最长等待时间（毫秒），超时返回 false（不更新 last_seq）。
     *
     * 超时不代表出错，仅用于主线程定期检查 stop 标志：
     *   while (...) {
     *       if (!cam.waitNewFrame(seq, 200)) continue;
     *       cam.readFrame(frame, &t1, &t2);
     *       // process frame ...
     *   }
     */
    bool waitNewFrame(uint64_t &last_seq, int timeout_ms = 200);

    /*
     * 将最新帧 clone 到 bgr，并输出时间戳（steady_clock 纳秒）：
     *   t1_ns  后台线程 cap.read() 完成后记录（T1）
     *   t2_ns  本函数帧 clone 完成后记录（T2）
     * 返回 false 表示尚无可用帧（极少发生，waitNewFrame 返回 true 后应立即调用）。
     */
    bool readFrame(cv::Mat &bgr,
                   int64_t *t1_ns = nullptr,
                   int64_t *t2_ns = nullptr);

    void close();
    bool isOpen() const { return running_.load(); }

private:
    /* 后台采集线程主体，持续 cap.read() + notify */
    void captureLoop();

    cv::VideoCapture cap_;

    std::thread             capture_thread_;
    std::atomic<bool>       running_{false};

    std::mutex              frame_mutex_;
    std::condition_variable frame_cv_;
    uint64_t                frame_seq_            = 0;
    cv::Mat                 latest_frame_;
    bool                    has_frame_            = false;
    int64_t                 latest_frame_time_ns_ = 0; /* T1 */
};
