#!/usr/bin/env python3
"""Publish USB-camera frames using the VelaBot XRAW v2 wire format.

The camera is requested in MJPEG mode so USB carries compressed frames. OpenCV
decodes those frames to BGR. The wire encoding is selectable so a raw-BGR run
and a JPEG run use the same capture and blocking PUB send path; this isolates
the effect of encoding without changing scheduling or queue policy.
"""

from __future__ import annotations

import argparse
import signal
import struct
import sys
import threading
import time
from dataclasses import dataclass

import cv2
import zmq

_HEADER = struct.Struct(">4sIIIIQQQ16s16sQI")
_MAGIC = b"XRAW"
_VERSION = 2
_MAX_TEXT = 16


@dataclass
class CameraStats:
    captured: int = 0
    encoded: int = 0
    sent: int = 0
    bytes_sent: int = 0
    last_seq: int = -1


@dataclass(frozen=True)
class CameraSpec:
    role: str
    device: str
    endpoint: str


def _text(value: str, field: str) -> bytes:
    encoded = value.encode("utf-8")
    if not encoded or len(encoded) > _MAX_TEXT:
        raise ValueError(f"{field} must be 1..{_MAX_TEXT} UTF-8 bytes: {value!r}")
    return encoded.ljust(_MAX_TEXT, b"\0")


def _parse_spec(value: str, option: str) -> tuple[str, str]:
    role, separator, target = value.partition("=")
    if not separator or not role.strip() or not target.strip():
        raise ValueError(f"{option} must be ROLE=VALUE, got {value!r}")
    return role.strip(), target.strip()


def _parse_specs(camera_values: list[str], bind_values: list[str]) -> tuple[CameraSpec, ...]:
    cameras = dict(_parse_spec(value, "--camera") for value in camera_values)
    binds = dict(_parse_spec(value, "--bind") for value in bind_values)
    if not cameras:
        raise ValueError("at least one --camera ROLE=/dev/videoN is required")
    if set(cameras) != set(binds):
        missing_camera = sorted(set(binds) - set(cameras))
        missing_bind = sorted(set(cameras) - set(binds))
        raise ValueError(f"camera/bind roles differ: camera-only={missing_bind}, bind-only={missing_camera}")
    return tuple(CameraSpec(role, cameras[role], binds[role]) for role in sorted(cameras))


def _xraw(role: str, encoding: str, seq: int, width: int, height: int, payload: bytes) -> bytes:
    now_wall = time.time_ns()
    now_mono = time.monotonic_ns()
    header = _HEADER.pack(
        _MAGIC,
        _VERSION,
        width,
        height,
        3,
        seq,
        now_wall,
        now_mono,
        _text(role, "role"),
        _text(encoding, "encoding"),
        now_mono,
        0,
    )
    return header + payload


def _worker(
    spec: CameraSpec,
    args: argparse.Namespace,
    context: zmq.Context,
    stop: threading.Event,
    stats: CameraStats,
) -> None:
    capture = cv2.VideoCapture(spec.device, cv2.CAP_V4L2)
    if not capture.isOpened():
        raise RuntimeError(f"{spec.role}: cannot open {spec.device}")

    capture.set(cv2.CAP_PROP_FOURCC, cv2.VideoWriter_fourcc(*"MJPG"))
    capture.set(cv2.CAP_PROP_FRAME_WIDTH, args.width)
    capture.set(cv2.CAP_PROP_FRAME_HEIGHT, args.height)
    capture.set(cv2.CAP_PROP_FPS, args.fps)

    actual_width = int(capture.get(cv2.CAP_PROP_FRAME_WIDTH))
    actual_height = int(capture.get(cv2.CAP_PROP_FRAME_HEIGHT))
    actual_fps = capture.get(cv2.CAP_PROP_FPS)
    if (actual_width, actual_height) != (args.width, args.height):
        raise RuntimeError(
            f"{spec.role}: camera selected {actual_width}x{actual_height}, requested {args.width}x{args.height}"
        )
    if actual_fps + 1e-3 < args.fps * 0.9:
        raise RuntimeError(f"{spec.role}: camera accepted only {actual_fps:.2f} FPS, requested {args.fps}")

    socket = context.socket(zmq.PUB)
    socket.bind(spec.endpoint)
    print(
        f"{spec.role}: {spec.device} -> {spec.endpoint}, "
        f"{actual_width}x{actual_height}@{actual_fps:.1f}, MJPG input / {args.encoding} ZMQ",
        flush=True,
    )

    try:
        while not stop.is_set():
            ok, frame = capture.read()
            if not ok:
                raise RuntimeError(f"{spec.role}: camera read failed")
            stats.captured += 1
            if args.encoding == "jpeg":
                ok, encoded = cv2.imencode(".jpg", frame, [int(cv2.IMWRITE_JPEG_QUALITY), int(args.jpeg_quality)])
                if not ok:
                    raise RuntimeError(f"{spec.role}: JPEG encode failed")
                stats.encoded += 1
                body = encoded.tobytes()
            else:
                body = frame.tobytes()
            payload = _xraw(spec.role, args.encoding, stats.last_seq + 1, actual_width, actual_height, body)
            socket.send(payload)
            stats.sent += 1
            stats.bytes_sent += len(payload)
            stats.last_seq += 1
    finally:
        socket.close(linger=0)
        capture.release()


def _report(specs: tuple[CameraSpec, ...], stats: dict[str, CameraStats], started: float) -> None:
    elapsed = max(time.monotonic() - started, 1e-6)
    for spec in specs:
        current = stats[spec.role]
        print(
            f"{spec.role}: captured={current.captured} encoded={current.encoded} "
            f"sent={current.sent} "
            f"capture_fps={current.captured / elapsed:.2f} "
            f"send_fps={current.sent / elapsed:.2f} "
            f"wire_mib_s={current.bytes_sent / elapsed / 1_048_576:.2f}",
            flush=True,
        )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--camera", action="append", default=[], metavar="ROLE=/dev/videoN")
    parser.add_argument("--bind", action="append", default=[], metavar="ROLE=tcp://*:PORT")
    parser.add_argument("--width", type=int, default=640)
    parser.add_argument("--height", type=int, default=480)
    parser.add_argument("--fps", type=float, default=60.0)
    parser.add_argument("--encoding", choices=("jpeg", "raw_bgr"), default="jpeg")
    parser.add_argument("--jpeg-quality", type=int, default=80)
    parser.add_argument("--duration", type=float, default=0.0, help="seconds; 0 means until Ctrl-C")
    args = parser.parse_args(argv)
    if args.width <= 0 or args.height <= 0 or args.fps <= 0 or not 1 <= args.jpeg_quality <= 100:
        parser.error("width, height and fps must be positive; jpeg-quality must be 1..100")
    specs = _parse_specs(args.camera, args.bind)

    stop = threading.Event()
    signal.signal(signal.SIGINT, lambda _signum, _frame: stop.set())
    signal.signal(signal.SIGTERM, lambda _signum, _frame: stop.set())
    if args.duration > 0:
        threading.Timer(args.duration, stop.set).start()

    context = zmq.Context()
    stats = {spec.role: CameraStats() for spec in specs}
    threads = [
        threading.Thread(target=_worker, args=(spec, args, context, stop, stats[spec.role]), daemon=True)
        for spec in specs
    ]
    started = time.monotonic()
    for thread in threads:
        thread.start()
    while any(thread.is_alive() for thread in threads) and not stop.is_set():
        time.sleep(1.0)
        _report(specs, stats, started)
    stop.set()
    for thread in threads:
        thread.join()
    _report(specs, stats, started)
    context.term()
    return 0


if __name__ == "__main__":
    sys.exit(main())
