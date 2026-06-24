#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import argparse
import time

import cv2


def parse_args():
    parser = argparse.ArgumentParser(
        description="Probe real OpenCV capture FPS without sender pipeline."
    )
    parser.add_argument("--device", default="/dev/video0", help="Video device path.")
    parser.add_argument(
        "--backend",
        choices=["v4l2", "default", "gstreamer"],
        default="v4l2",
        help="OpenCV backend.",
    )
    parser.add_argument("--width", type=int, default=640, help="Requested width.")
    parser.add_argument("--height", type=int, default=480, help="Requested height.")
    parser.add_argument("--fps", type=int, default=30, help="Requested FPS.")
    parser.add_argument(
        "--fourcc",
        default="MJPG",
        help="Requested FOURCC, e.g. MJPG or YUYV.",
    )
    parser.add_argument(
        "--buffersize",
        type=int,
        default=1,
        help="Requested OpenCV CAP_PROP_BUFFERSIZE.",
    )
    parser.add_argument(
        "--count",
        type=int,
        default=300,
        help="Frames to read before stopping.",
    )
    parser.add_argument(
        "--report-every",
        type=int,
        default=60,
        help="Print interval FPS every N frames.",
    )
    return parser.parse_args()


def fourcc_to_str(value: float) -> str:
    code = int(value)
    chars = [
        chr(code & 0xFF),
        chr((code >> 8) & 0xFF),
        chr((code >> 16) & 0xFF),
        chr((code >> 24) & 0xFF),
    ]
    return "".join(chars)


def open_capture(device: str, backend: str, width: int, height: int, fps: int, fourcc: str):
    if backend == "gstreamer":
        fmt = (fourcc or "").upper()
        if fmt == "MJPG":
            src_caps = f"image/jpeg,width={width},height={height},framerate={fps}/1"
            decode = "jpegdec ! "
        elif fmt == "YUYV":
            src_caps = f"video/x-raw,format=YUY2,width={width},height={height},framerate={fps}/1"
            decode = ""
        else:
            raise SystemExit("gstreamer backend currently supports only MJPG or YUYV")
        pipeline = (
            f"v4l2src device={device} io-mode=2 do-timestamp=true ! "
            f"{src_caps} ! "
            f"{decode}videoconvert ! video/x-raw,format=BGR ! "
            "appsink drop=true max-buffers=1 sync=false"
        )
        print(f"[probe] gstreamer pipeline: {pipeline}")
        return cv2.VideoCapture(pipeline, cv2.CAP_GSTREAMER)
    if backend == "v4l2":
        return cv2.VideoCapture(device, cv2.CAP_V4L2)
    return cv2.VideoCapture(device)


def main():
    args = parse_args()

    cap = open_capture(
        args.device,
        args.backend,
        args.width,
        args.height,
        args.fps,
        args.fourcc,
    )
    if not cap.isOpened():
        raise SystemExit(f"failed to open {args.device} with backend={args.backend}")

    if args.fourcc:
        if len(args.fourcc) != 4:
            raise SystemExit("--fourcc must be exactly 4 characters")
        cap.set(
            cv2.CAP_PROP_FOURCC,
            cv2.VideoWriter.fourcc(*args.fourcc),
        )
    cap.set(cv2.CAP_PROP_FRAME_WIDTH, args.width)
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, args.height)
    cap.set(cv2.CAP_PROP_FPS, args.fps)
    cap.set(cv2.CAP_PROP_BUFFERSIZE, args.buffersize)

    actual_w = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    actual_h = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    actual_fps = float(cap.get(cv2.CAP_PROP_FPS))
    actual_fourcc = fourcc_to_str(cap.get(cv2.CAP_PROP_FOURCC))

    print(
        "[probe] opened"
        f" device={args.device}"
        f" backend={args.backend}"
        f" request={args.width}x{args.height}@{args.fps}"
        f" fourcc={args.fourcc}"
        f" buffersize={args.buffersize}"
    )
    print(
        "[probe] actual"
        f" size={actual_w}x{actual_h}"
        f" fps={actual_fps:.3f}"
        f" fourcc={actual_fourcc}"
    )

    total_count = 0
    report_count = 0
    total_start = time.perf_counter()
    report_start = total_start

    while total_count < args.count:
        ok, frame = cap.read()
        if not ok or frame is None or frame.size == 0:
            print(f"[probe] read failed at frame={total_count}")
            break

        total_count += 1
        report_count += 1

        if report_count >= args.report_every:
            now = time.perf_counter()
            elapsed = now - report_start
            fps = report_count / elapsed if elapsed > 0 else 0.0
            print(
                "[probe]"
                f" frames={total_count}"
                f" interval_fps={fps:.3f}"
                f" shape={frame.shape[1]}x{frame.shape[0]}x{frame.shape[2] if frame.ndim == 3 else 1}"
            )
            report_start = now
            report_count = 0

    total_elapsed = time.perf_counter() - total_start
    total_fps = total_count / total_elapsed if total_elapsed > 0 else 0.0
    print(
        "[probe] summary"
        f" frames={total_count}"
        f" elapsed={total_elapsed:.3f}s"
        f" fps={total_fps:.3f}"
    )
    cap.release()


if __name__ == "__main__":
    main()
