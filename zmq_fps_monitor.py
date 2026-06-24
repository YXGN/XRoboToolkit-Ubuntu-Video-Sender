#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import argparse
import time

from zmq_receiver import (
    EncodedPacket,
    RawFramePacket,
    ZMQReceiver,
    detect_message,
    format_latency,
)


def run_monitor(receiver: ZMQReceiver, mode: str, interval_sec: float):
    total_frames = 0
    interval_frames = 0
    last_report_time = time.time()
    last_frame_id = None
    dropped_frame_hint = 0

    print(
        f"[monitor] mode={mode} interval={interval_sec:.1f}s "
        "Ctrl+C to stop"
    )

    try:
        while True:
            message = receiver.recv(1000)
            if message is None:
                now = time.time()
                if now - last_report_time >= interval_sec and total_frames == 0:
                    print("[monitor] waiting for frames...")
                    last_report_time = now
                continue

            kind, packet = detect_message(message, mode)
            if kind == "raw":
                pkt: RawFramePacket = packet
                frame_id = int(pkt.frame_id)
                payload_desc = f"{pkt.width}x{pkt.height}x{pkt.channels}"
                cap_age = format_latency(int(time.time() * 1_000_000), pkt.capture_utc_us)
                send_age = format_latency(int(time.time() * 1_000_000), pkt.send_utc_us)
            else:
                pkt = packet  # type: EncodedPacket
                frame_id = int(pkt.frame_id)
                payload_desc = f"payload={pkt.payload_size}"
                cap_age = format_latency(int(time.time() * 1_000_000), pkt.capture_utc_us)
                send_age = format_latency(int(time.time() * 1_000_000), pkt.send_utc_us)

            total_frames += 1
            interval_frames += 1

            if last_frame_id is not None and frame_id > last_frame_id + 1:
                dropped_frame_hint += frame_id - last_frame_id - 1
            last_frame_id = frame_id

            now = time.time()
            elapsed = now - last_report_time
            if elapsed >= interval_sec:
                fps = interval_frames / elapsed if elapsed > 0 else 0.0
                print(
                    "[monitor]"
                    f" fps={fps:.2f}"
                    f" total={total_frames}"
                    f" frame={frame_id}"
                    f" dropped_hint={dropped_frame_hint}"
                    f" {payload_desc}"
                    f" cap_age={cap_age}"
                    f" send_age={send_age}"
                )
                interval_frames = 0
                last_report_time = now
    except KeyboardInterrupt:
        pass
    finally:
        print(
            f"[monitor] stopped total_frames={total_frames}"
            f" dropped_hint={dropped_frame_hint}"
        )


def main():
    parser = argparse.ArgumentParser(description="Headless ZMQ FPS monitor")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=5556)
    parser.add_argument(
        "--mode",
        choices=["auto", "raw", "encoded"],
        default="raw",
        help="Packet mode to decode",
    )
    parser.add_argument(
        "--interval",
        type=float,
        default=1.0,
        help="Seconds between fps reports",
    )
    args = parser.parse_args()

    endpoint = f"tcp://{args.host}:{args.port}"
    receiver = ZMQReceiver(endpoint)
    try:
        run_monitor(receiver, args.mode, args.interval)
    finally:
        receiver.close()


if __name__ == "__main__":
    main()
