#!/usr/bin/env python3
"""
test_bitrate_monitor.py — 发送端码率稳定性 & 画质健康度测试工具

用法:
  python3 test_bitrate_monitor.py --port 9000 --window 3 --duration 60

  ./OrinVideoSender --send --server 127.0.0.1 --port 9000 \
      --width 2560 --height 720 --fps 60 --bitrate 4000000 \
      --stereo-camera /dev/video0

工作原理:
  监听指定 TCP 端口，等待 OrinVideoSender（或任何遵循
  「大端 4 字节长度 + payload」协议的发送端）连接后，
  持续统计每秒实际传输码率、帧大小与帧间抖动，
  并在终端以滚动方式显示实时报告与汇总统计。

协议格式（与 zed_webcam_common.cpp 中 on_new_sample 一致）:
  [uint32_be: payload_size][payload_size bytes: encoded AU]

【拖影诊断说明】
  码率稳定 ≠ 画质稳定。intra-refresh=true 配合长 GOP（fps×4）时，
  编码器会将 I 帧刷新分散到整个 GOP 周期，快速运动场景下帧大小均匀
  但参考块陈旧，导致局部拖影长达数秒。

  本工具新增「低帧连串」检测：连续多帧帧大小低于均值的 40% 时，
  说明编码器在大量复用旧参考块（低复杂度 P 帧），这是拖影风险的
  直接信号。建议将 zed_webcam_common.cpp 中的 key_int_max 从
  fps×4 缩短为 fps（1 秒 GOP），并移除 intra-refresh=true。

"""

import argparse
import socket
import struct
import sys
import time
import collections
import math
import signal
import threading

# ─────────────────────────── 工具函数 ────────────────────────────

def fmt_bps(bps: float) -> str:
    """将 bps 格式化为易读字符串。"""
    if bps >= 1e6:
        return f"{bps / 1e6:.2f} Mbps"
    if bps >= 1e3:
        return f"{bps / 1e3:.1f} Kbps"
    return f"{bps:.0f} bps"

def fmt_bytes(b: int) -> str:
    if b >= 1024 * 1024:
        return f"{b / 1024 / 1024:.2f} MB"
    if b >= 1024:
        return f"{b / 1024:.1f} KB"
    return f"{b} B"

def stddev(values):
    n = len(values)
    if n < 2:
        return 0.0
    mean = sum(values) / n
    return math.sqrt(sum((v - mean) ** 2 for v in values) / n)

# ─────────────────────────── 接收逻辑 ────────────────────────────

def recv_exact(sock: socket.socket, n: int) -> bytes:
    """阻塞读取精确 n 字节，连接断开时返回 b''。"""
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            return b""
        buf.extend(chunk)
    return bytes(buf)

# ─────────────────────────── 主测试类 ────────────────────────────

class BitrateMonitor:
    def __init__(self, port: int, window: int, duration: int):
        self.port = port
        self.window = window          # 滑动窗口大小（秒）
        self.duration = duration      # 0 = 无限
        self._stop = threading.Event()

        # 每秒快照：(timestamp, bytes_in_that_second)
        self._sec_bytes: collections.deque = collections.deque()
        # 帧列表：(timestamp, frame_size_bytes)
        self._frames: list = []
        self._total_bytes = 0
        self._total_frames = 0
        self._start_ts: float = 0.0

        # 拖影风险检测：记录「低帧连串」事件 (start_ts, length_frames)
        self._ghost_events: list = []
        self._low_run_start: float = 0.0
        self._low_run_len: int = 0
        # 低帧阈值系数：帧大小低于历史均值该比例时视为"低复杂度 P 帧"
        self._low_frame_ratio: float = 0.40
        # 触发告警的连续低帧数
        self._ghost_run_threshold: int = 8

    # ── 信号处理 ──────────────────────────────────────────────────
    def _handle_sigint(self, *_):
        print("\n[monitor] 收到 Ctrl+C，正在退出…")
        self._stop.set()

    # ── 拖影风险检测（在接收线程里每帧调用）────────────────────────
    def _check_ghost_risk(self, frame_size: int, now: float):
        """
        用滑动历史均值判断当前帧是否为"低复杂度 P 帧"。
        连续超过阈值帧数时记录一次拖影风险事件并打印警告。
        """
        # 用最近 5 秒全部帧计算基准均值（至少需要 10 帧）
        baseline_frames = [sz for ts, sz in self._frames
                           if now - ts <= 5.0]
        if len(baseline_frames) < 10:
            return
        baseline_avg = sum(baseline_frames) / len(baseline_frames)
        threshold = baseline_avg * self._low_frame_ratio

        if frame_size < threshold:
            if self._low_run_len == 0:
                self._low_run_start = now
            self._low_run_len += 1
            if self._low_run_len == self._ghost_run_threshold:
                print(
                    f"\n  ⚠  [拖影风险] 连续 {self._low_run_len} 帧帧大小"
                    f" < {self._low_frame_ratio*100:.0f}% 均值"
                    f"（当前 {fmt_bytes(frame_size)} vs 均值 {fmt_bytes(int(baseline_avg))}）"
                    f" — 编码器可能在大量复用旧参考块"
                )
        else:
            if self._low_run_len >= self._ghost_run_threshold:
                duration_ms = (now - self._low_run_start) * 1000
                self._ghost_events.append((self._low_run_start, self._low_run_len))
                print(
                    f"  ✓  [拖影风险解除] 低帧连串结束，持续 {duration_ms:.0f} ms"
                    f" / {self._low_run_len} 帧\n"
                )
            self._low_run_len = 0

    # ── 统计打印线程 ──────────────────────────────────────────────
    def _print_loop(self):
        prev_bytes = 0
        prev_ts = time.monotonic()

        while not self._stop.is_set():
            time.sleep(1.0)
            now = time.monotonic()
            dt = now - prev_ts
            cur_bytes = self._total_bytes

            interval_bytes = cur_bytes - prev_bytes
            interval_bps = (interval_bytes * 8) / dt if dt > 0 else 0

            # 滑动窗口码率
            window_entries = [b for ts, b in self._sec_bytes
                              if now - ts <= self.window]
            win_bps = (sum(window_entries) * 8) / self.window if window_entries else 0

            # 帧大小统计（最近 window 秒内）
            recent_frames = [sz for ts, sz in self._frames
                             if now - ts <= self.window]
            if recent_frames:
                frame_min = min(recent_frames)
                frame_max = max(recent_frames)
                frame_avg = sum(recent_frames) / len(recent_frames)
                frame_sd  = stddev(recent_frames)
                # 帧大小峰谷比：反映 IDR 帧与 P 帧的大小差异
                peak_valley_ratio = frame_max / frame_min if frame_min > 0 else 0
            else:
                frame_min = frame_max = frame_avg = frame_sd = 0
                peak_valley_ratio = 0

            # 帧率（最近 window 秒）
            fps_recent = len(recent_frames) / self.window

            # 帧间抖动（最近 window 秒）
            recent_ts = sorted(ts for ts, _ in self._frames
                               if now - ts <= self.window)
            if len(recent_ts) >= 2:
                gaps = [recent_ts[i+1] - recent_ts[i]
                        for i in range(len(recent_ts)-1)]
                jitter_ms = stddev(gaps) * 1000
                avg_gap_ms = (sum(gaps) / len(gaps)) * 1000
            else:
                jitter_ms = avg_gap_ms = 0.0

            elapsed = now - self._start_ts if self._start_ts else 0

            # 拖影风险状态标记
            ghost_tag = ""
            if self._low_run_len >= self._ghost_run_threshold:
                ghost_tag = f"  ⚠ 低帧连串:{self._low_run_len}帧"

            print(
                f"[{elapsed:6.1f}s] "
                f"瞬时: {fmt_bps(interval_bps):>12s}  "
                f"窗口({self.window}s)均值: {fmt_bps(win_bps):>12s}  "
                f"帧率: {fps_recent:5.1f} fps  "
                f"帧大小 avg/min/max/σ: "
                f"{fmt_bytes(int(frame_avg))}/{fmt_bytes(frame_min)}/{fmt_bytes(frame_max)}/{fmt_bytes(int(frame_sd))}  "
                f"峰谷比: {peak_valley_ratio:4.1f}x  "
                f"帧间抖动σ: {jitter_ms:.1f} ms  avg间隔: {avg_gap_ms:.1f} ms"
                f"{ghost_tag}"
            )

            self._sec_bytes.append((now, interval_bytes))
            # 只保留最近 60 秒
            while self._sec_bytes and now - self._sec_bytes[0][0] > 60:
                self._sec_bytes.popleft()

            prev_bytes = cur_bytes
            prev_ts = now

            if self.duration > 0 and elapsed >= self.duration:
                print("[monitor] 已达到指定测试时长，停止。")
                self._stop.set()

    # ── 摘要报告 ──────────────────────────────────────────────────
    def _print_summary(self):
        elapsed = time.monotonic() - self._start_ts if self._start_ts else 0
        print("\n" + "=" * 70)
        print("  码率稳定性汇总报告")
        print("=" * 70)
        print(f"  测试时长      : {elapsed:.1f} s")
        print(f"  总接收字节    : {fmt_bytes(self._total_bytes)}")
        print(f"  总帧数        : {self._total_frames}")
        if elapsed > 0 and self._total_bytes > 0:
            avg_bps = (self._total_bytes * 8) / elapsed
            print(f"  全程平均码率  : {fmt_bps(avg_bps)}")
        if self._sec_bytes:
            sec_bps_list = [b * 8 for _, b in self._sec_bytes]
            print(f"  逐秒码率 最大 : {fmt_bps(max(sec_bps_list))}")
            print(f"  逐秒码率 最小 : {fmt_bps(min(sec_bps_list))}")
            print(f"  逐秒码率 σ   : {fmt_bps(stddev(sec_bps_list))}")
            mean_bps = sum(sec_bps_list) / len(sec_bps_list) if sec_bps_list else 0
            cv = (stddev(sec_bps_list) / mean_bps * 100) if mean_bps > 0 else 0
            print(f"  变异系数(CV)  : {cv:.1f}%  (越低越稳定)")
        if self._frames:
            sizes = [sz for _, sz in self._frames]
            avg_sz = sum(sizes) / len(sizes)
            print(f"  帧大小 avg    : {fmt_bytes(int(avg_sz))}")
            print(f"  帧大小 σ      : {fmt_bytes(int(stddev(sizes)))}")
            print(f"  帧大小 max/min: {fmt_bytes(max(sizes))} / {fmt_bytes(min(sizes))}"
                  f"  峰谷比: {max(sizes)/min(sizes):.1f}x"
                  f"  (正常 IDR+P 流: 3x~10x，intra-refresh 流: 接近 1x)")
            ts_list = sorted(t for t, _ in self._frames)
            if len(ts_list) >= 2:
                gaps = [(ts_list[i+1]-ts_list[i])*1000 for i in range(len(ts_list)-1)]
                print(f"  帧间隔 avg    : {sum(gaps)/len(gaps):.1f} ms")
                print(f"  帧间隔 σ(抖动): {stddev(gaps):.1f} ms")
                print(f"  帧间隔 最大   : {max(gaps):.1f} ms")
        # 拖影风险事件汇总
        print(f"\n  【拖影风险事件】共检测到 {len(self._ghost_events)} 次低帧连串")
        if self._ghost_events:
            for i, (ts, length) in enumerate(self._ghost_events, 1):
                offset = ts - self._start_ts if self._start_ts else 0
                print(f"    #{i}: t={offset:.1f}s  持续 {length} 帧")
            print(f"  提示: 若事件频繁，请确认 zed_webcam_common.cpp 中")
            print(f"        key_int_max=fps（非 fps*4）且已移除 intra-refresh=true")
        else:
            print(f"  ✓ 未检测到明显拖影风险（低帧连串 < {self._ghost_run_threshold} 帧）")
        print("=" * 70)

    # ── 主入口 ────────────────────────────────────────────────────
    def run(self):
        signal.signal(signal.SIGINT, self._handle_sigint)

        server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind(("0.0.0.0", self.port))
        server.listen(1)
        server.settimeout(1.0)

        print(f"[monitor] 监听 TCP 0.0.0.0:{self.port}，等待发送端连接…")
        print(f"          统计窗口: {self.window}s  测试时长: "
              f"{'无限' if self.duration == 0 else str(self.duration)+'s'}")
        print(f"          按 Ctrl+C 停止并查看汇总报告\n")

        conn = None
        while not self._stop.is_set():
            try:
                conn, addr = server.accept()
                print(f"[monitor] 发送端已连接: {addr[0]}:{addr[1]}")
                break
            except socket.timeout:
                continue

        if conn is None:
            server.close()
            return

        conn.settimeout(2.0)
        self._start_ts = time.monotonic()

        # 启动统计打印线程
        printer = threading.Thread(target=self._print_loop, daemon=True)
        printer.start()

        try:
            while not self._stop.is_set():
                # 读 4 字节长度头
                header = recv_exact(conn, 4)
                if not header:
                    print("[monitor] 连接已断开（length header 为空）")
                    break
                (payload_size,) = struct.unpack(">I", header)

                if payload_size == 0 or payload_size > 100 * 1024 * 1024:
                    print(f"[monitor] 异常 payload_size={payload_size}，跳过")
                    break

                # 读 payload（直接丢弃，只计字节）
                payload = recv_exact(conn, payload_size)
                if len(payload) != payload_size:
                    print("[monitor] payload 不完整，连接断开")
                    break

                now = time.monotonic()
                total_frame = 4 + payload_size
                self._total_bytes += total_frame
                self._total_frames += 1
                self._frames.append((now, total_frame))
                self._check_ghost_risk(total_frame, now)

        except socket.timeout:
            print("[monitor] 超时：发送端超过 2 秒无数据")
        except Exception as e:
            print(f"[monitor] 接收异常: {e}")
        finally:
            self._stop.set()
            conn.close()
            server.close()

        printer.join(timeout=2)
        self._print_summary()


# ─────────────────────────── CLI ────────────────────────────────

def main():
    ap = argparse.ArgumentParser(
        description="OrinVideoSender 发送端码率稳定性测试工具")
    ap.add_argument("--port", type=int, default=9000,
                    help="监听端口，需与 --port 参数保持一致（默认 9000）")
    ap.add_argument("--window", type=int, default=3,
                    help="滑动统计窗口大小（秒，默认 3）")
    ap.add_argument("--duration", type=int, default=0,
                    help="测试时长（秒），0 表示无限（默认 0）")
    args = ap.parse_args()

    monitor = BitrateMonitor(
        port=args.port,
        window=args.window,
        duration=args.duration,
    )
    monitor.run()

if __name__ == "__main__":
    main()
