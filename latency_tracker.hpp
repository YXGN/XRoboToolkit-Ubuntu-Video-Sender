/*
 * latency_tracker.hpp
 *
 * 采集→编码→发送各阶段延迟统计（header-only，单例）。
 *
 * 阶段划分：
 *   T1  libuvc frameCallback 收到帧
 *   T2  readBgr 读完 latest_jpeg_（锁释放后）
 *   T3  cv::imdecode 完成
 *   T4  cv::resize 完成
 *   T5  gst_app_src_push_buffer 返回
 *   T6  on_new_sample 进入（GStreamer 编码线程）
 *   T7  sendData 返回（TCP 发送完成）
 *
 * 用法：
 *   // streaming 线程（T1~T5）：
 *   LatencyTracker::get().add_capture(t1, t2, t3, t4, t5);
 *
 *   // GStreamer appsink 线程（T6~T7），T5 通过原子变量跨线程传递：
 *   int64_t t5 = LatencyTracker::get().last_push_ns.load();
 *   // ... send ...
 *   LatencyTracker::get().add_encode_send(t5, t6, t7);
 *
 * 每 REPORT_INTERVAL 帧自动打印一次统计，之后重置计数器。
 */
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>

/* ── 时钟辅助 ── */
inline int64_t lat_now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/* ── 单阶段统计 ── */
struct LatencyStageStat {
    double sum_ms = 0.0;
    double min_ms = 1e9;
    double max_ms = 0.0;
    int    count  = 0;

    void add(double ms) {
        sum_ms += ms;
        if (ms < min_ms) min_ms = ms;
        if (ms > max_ms) max_ms = ms;
        ++count;
    }
    double avg() const { return count ? sum_ms / count : 0.0; }
    void reset() { sum_ms = 0.0; min_ms = 1e9; max_ms = 0.0; count = 0; }
};

/* ── 全局追踪器（单例）── */
class LatencyTracker {
public:
    /* 每隔多少帧打印一次报告 */
    static constexpr int REPORT_INTERVAL = 30;

    static LatencyTracker& get() {
        static LatencyTracker inst;
        return inst;
    }

    /*
     * streaming 线程调用：记录 T1~T5。
     *   T1→T2  等待新帧（从上帧回调到本次 readBgr 读到数据）
     *   T2→T3  JPEG 软解码（cv::imdecode）
     *   T3→T4  resize
     *   T4→T5  appsrc push 耗时（含 queue 积压阻塞）
     */
    void add_capture(int64_t t1, int64_t t2, int64_t t3, int64_t t4, int64_t t5) {
        std::lock_guard<std::mutex> lk(mu_);
        s_wait_  .add((t2 - t1) / 1e6);
        s_decode_.add((t3 - t2) / 1e6);
        s_resize_.add((t4 - t3) / 1e6);
        s_push_  .add((t5 - t4) / 1e6);
        ++cap_count_;
        try_print_locked();
    }

    /*
     * GStreamer appsink 线程调用：记录 T5（从原子变量）、T6、T7。
     *   T5→T6  GStreamer 编码耗时（从 push 到 appsink 回调被触发）
     *   T6→T7  TCP 同步发送耗时（含网络阻塞）
     */
    void add_encode_send(int64_t t5, int64_t t6, int64_t t7) {
        std::lock_guard<std::mutex> lk(mu_);
        s_encode_.add((t6 - t5) / 1e6);
        s_send_  .add((t7 - t6) / 1e6);
        ++enc_count_;
        try_print_locked();
    }

    /*
     * T5→T6 跨线程桥梁：
     *   streaming 线程在 gst_app_src_push_buffer 返回后写入；
     *   on_new_sample 进入时读取。
     *
     * 注意：这是近似值（queue 可能缓存多帧），但作为均值统计足够。
     */
    std::atomic<int64_t> last_push_ns{0};

private:
    LatencyTracker() = default;

    std::mutex mu_;
    int cap_count_ = 0;
    int enc_count_ = 0;

    LatencyStageStat s_wait_;
    LatencyStageStat s_decode_;
    LatencyStageStat s_resize_;
    LatencyStageStat s_push_;
    LatencyStageStat s_encode_;
    LatencyStageStat s_send_;

    /* 在持锁状态下检查是否达到打印条件 */
    void try_print_locked() {
        if (cap_count_ < REPORT_INTERVAL || enc_count_ < REPORT_INTERVAL)
            return;

        /* 计算各阶段之和作为估算端到端延迟 */
        double total_avg = s_wait_.avg() + s_decode_.avg() + s_resize_.avg()
                         + s_push_.avg() + s_encode_.avg() + s_send_.avg();

        std::printf(
            "\n╔══ [Latency Stats | %d frames] ══════════════════════════════╗\n"
            "║  T1→T2  等待新帧      : avg=%7.2f ms  min=%7.2f  max=%7.2f ║\n"
            "║  T2→T3  JPEG 解码     : avg=%7.2f ms  min=%7.2f  max=%7.2f ║\n"
            "║  T3→T4  resize        : avg=%7.2f ms  min=%7.2f  max=%7.2f ║\n"
            "║  T4→T5  appsrc push   : avg=%7.2f ms  min=%7.2f  max=%7.2f ║\n"
            "║  T5→T6  GStreamer编码  : avg=%7.2f ms  min=%7.2f  max=%7.2f ║\n"
            "║  T6→T7  TCP 发送      : avg=%7.2f ms  min=%7.2f  max=%7.2f ║\n"
            "║  ─────────────────────────────────────────────────────────── ║\n"
            "║  估算端到端           : avg=%7.2f ms                         ║\n"
            "╚═════════════════════════════════════════════════════════════╝\n",
            REPORT_INTERVAL,
            s_wait_.avg(),   s_wait_.min_ms,   s_wait_.max_ms,
            s_decode_.avg(), s_decode_.min_ms, s_decode_.max_ms,
            s_resize_.avg(), s_resize_.min_ms, s_resize_.max_ms,
            s_push_.avg(),   s_push_.min_ms,   s_push_.max_ms,
            s_encode_.avg(), s_encode_.min_ms, s_encode_.max_ms,
            s_send_.avg(),   s_send_.min_ms,   s_send_.max_ms,
            total_avg
        );
        std::fflush(stdout);

        /* 重置所有计数器，开始下一个统计窗口 */
        s_wait_.reset();  s_decode_.reset(); s_resize_.reset();
        s_push_.reset();  s_encode_.reset(); s_send_.reset();
        cap_count_ = 0;
        enc_count_ = 0;
    }
};
