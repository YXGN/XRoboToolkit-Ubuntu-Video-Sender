/*
 * latency_tracker.hpp  —  Webcam 版
 *
 * 采集→编码→发送各阶段延迟统计（header-only，单例）。
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * 阶段划分（cv::VideoCapture + 后台采集线程）：
 *
 *   T1  后台采集线程 cap.read() 返回，帧从 V4L2 驱动出来（帧采集时刻）
 *   T2  主线程从 waitNewFrame() 醒来并完成帧拷贝（主线程首次持有该帧）
 *   T3  cv::cvtColor BGR→BGRA 完成
 *   T4  cv::resize 完成，out_bgra 就绪
 *   T5  gst_app_src_push_buffer 返回（若 queue 已满会在此阻塞）
 *   T6  on_new_sample 回调进入（GStreamer 编码线程被触发）
 *   T7  sendData 返回（TCP 发送完成，含网络阻塞）
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * 关键阶段含义：
 *
 *   T1→T2  帧投递延迟：后台线程采集到帧 → 主线程拿到帧
 *          • 反映条件变量唤醒 + 帧 clone 的开销，正常应在 0-2 ms 内
 *          • 若偏高，说明主线程处理速度跟不上（编码/push 阻塞溢出到这里）
 *
 *   T2→T3  BGR→BGRA 色彩转换耗时
 *          • 软件 cvtColor，随分辨率线性增长；1080p 约 1-3 ms
 *
 *   T3→T4  cv::resize 耗时
 *          • 双线性插值；分辨率差越大耗时越高
 *
 *   T4→T5  appsrc push 耗时（★ queue 优化效果可见处）
 *          • 优化前（queue 默认 200 帧）：queue 积压时此处可能阻塞数十 ms
 *          • 优化后（queue max=1 leaky=downstream）：几乎为 0 ms（立即返回）
 *
 *   T5→T6  GStreamer x264/x265 编码耗时
 *          • 从 push 完成到 appsink 回调被触发；含编码 + queue 传递
 *          • zerolatency + ultrafast 下约 10-30 ms（取决于分辨率与 CPU）
 *
 *   T6→T7  TCP 同步发送耗时
 *          • 含网络 RTT 的阻塞等待（send() syscall 直到内核缓冲接受数据）
 *          • 局域网通常 < 1 ms；跨网段或带宽满载时会升高
 *
 *   估算端到端 ≈ T1→T7 各段之和（不含网络传播时延与接收端解码）
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * 用法：
 *   // streaming 线程（T1~T5）：
 *   LatencyTracker::get().add_capture(t1, t2, t3, t4, t5);
 *
 *   // GStreamer appsink 线程（T6~T7），T5 通过原子变量跨线程传递：
 *   int64_t t5 = LatencyTracker::get().last_push_ns.load();
 *   // ... sendData ...
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
     *   T1→T2  帧投递（后台采集线程 → 主线程拿到帧）
     *   T2→T3  BGR→BGRA 色彩转换
     *   T3→T4  resize
     *   T4→T5  appsrc push 耗时（queue 优化效果在此体现）
     */
    void add_capture(int64_t t1, int64_t t2, int64_t t3, int64_t t4, int64_t t5) {
        std::lock_guard<std::mutex> lk(mu_);
        s_deliver_.add((t2 - t1) / 1e6);
        s_cvt_   .add((t3 - t2) / 1e6);
        s_resize_.add((t4 - t3) / 1e6);
        s_push_  .add((t5 - t4) / 1e6);
        ++cap_count_;
        try_print_locked();
    }

    /*
     * GStreamer appsink 线程调用：记录 T5（从原子变量）、T6、T7。
     *   T5→T6  GStreamer 编码耗时
     *   T6→T7  TCP 同步发送耗时
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

    LatencyStageStat s_deliver_;  /* T1→T2 */
    LatencyStageStat s_cvt_;      /* T2→T3 */
    LatencyStageStat s_resize_;   /* T3→T4 */
    LatencyStageStat s_push_;     /* T4→T5 */
    LatencyStageStat s_encode_;   /* T5→T6 */
    LatencyStageStat s_send_;     /* T6→T7 */

    void try_print_locked() {
        if (cap_count_ < REPORT_INTERVAL || enc_count_ < REPORT_INTERVAL)
            return;

        double total_avg = s_deliver_.avg() + s_cvt_.avg()  + s_resize_.avg()
                         + s_push_.avg()   + s_encode_.avg() + s_send_.avg();

        std::printf(
            "\n╔══ [Latency Stats | %d frames] ══════════════════════════════╗\n"
            "║  T1→T2  帧投递（采集→主线程）: avg=%7.2f ms  min=%6.2f  max=%6.2f ║\n"
            "║  T2→T3  BGR→BGRA 色彩转换   : avg=%7.2f ms  min=%6.2f  max=%6.2f ║\n"
            "║  T3→T4  resize              : avg=%7.2f ms  min=%6.2f  max=%6.2f ║\n"
            "║  T4→T5  appsrc push         : avg=%7.2f ms  min=%6.2f  max=%6.2f ║\n"
            "║  T5→T6  GStreamer 编码       : avg=%7.2f ms  min=%6.2f  max=%6.2f ║\n"
            "║  T6→T7  TCP 发送            : avg=%7.2f ms  min=%6.2f  max=%6.2f ║\n"
            "║  ─────────────────────────────────────────────────────────────── ║\n"
            "║  估算端到端                 : avg=%7.2f ms                        ║\n"
            "╚═════════════════════════════════════════════════════════════════╝\n",
            REPORT_INTERVAL,
            s_deliver_.avg(), s_deliver_.min_ms, s_deliver_.max_ms,
            s_cvt_.avg(),     s_cvt_.min_ms,     s_cvt_.max_ms,
            s_resize_.avg(),  s_resize_.min_ms,  s_resize_.max_ms,
            s_push_.avg(),    s_push_.min_ms,    s_push_.max_ms,
            s_encode_.avg(),  s_encode_.min_ms,  s_encode_.max_ms,
            s_send_.avg(),    s_send_.min_ms,    s_send_.max_ms,
            total_avg
        );
        std::fflush(stdout);

        s_deliver_.reset(); s_cvt_.reset();    s_resize_.reset();
        s_push_.reset();    s_encode_.reset(); s_send_.reset();
        cap_count_ = 0;
        enc_count_ = 0;
    }
};
