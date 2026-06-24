#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import argparse
import struct
import time
from dataclasses import dataclass
from typing import Optional, Tuple

import cv2
import numpy as np
import zmq


RAW_MAGIC = b"XRRM"
RAW_PIXEL_FORMAT_BGRA8 = 1
RAW_HEADER_FMT = "<4sHHQQQIIIIII"
RAW_HEADER_SIZE = struct.calcsize(RAW_HEADER_FMT)

XRAW_MAGIC = b"XRAW"
XRAW_HEADER_FMT = ">4sIIIIQQQ"
XRAW_HEADER_SIZE = struct.calcsize(XRAW_HEADER_FMT)

ENC_MAGIC = b"XRLT"
ENC_OUTER_HEADER_SIZE = 4
ENC_HEADER_FMT = "<4sHHQQQII"
ENC_HEADER_SIZE = struct.calcsize(ENC_HEADER_FMT)


@dataclass
class RawFramePacket:
    frame_id: int
    capture_utc_us: int
    send_utc_us: int
    width: int
    height: int
    channels: int
    stride_bytes: int
    payload_size: int
    pixel_format: int
    image: np.ndarray


@dataclass
class EncodedPacket:
    frame_id: int
    capture_utc_us: int
    send_utc_us: int
    payload_size: int
    payload: bytes


def parse_raw_frame(message: bytes) -> Optional[RawFramePacket]:
    if len(message) < RAW_HEADER_SIZE:
        return None

    (
        magic,
        version,
        header_size,
        frame_id,
        capture_utc_us,
        send_utc_us,
        width,
        height,
        channels,
        stride_bytes,
        payload_size,
        pixel_format,
    ) = struct.unpack_from(RAW_HEADER_FMT, message, 0)

    if magic != RAW_MAGIC or version != 1 or header_size != RAW_HEADER_SIZE:
        return None

    if pixel_format != RAW_PIXEL_FORMAT_BGRA8:
        raise ValueError(f"unsupported raw pixel_format={pixel_format}")

    if channels not in (3, 4):
        raise ValueError(f"unsupported raw channels={channels}")

    if len(message) != header_size + payload_size:
        raise ValueError(
            f"raw packet size mismatch: expect {header_size + payload_size}, got {len(message)}"
        )

    expected_min_stride = width * channels
    if stride_bytes < expected_min_stride:
        raise ValueError(
            f"invalid stride_bytes={stride_bytes}, expect >= {expected_min_stride}"
        )
    if payload_size != stride_bytes * height:
        raise ValueError(
            f"invalid payload_size={payload_size}, expect {stride_bytes * height}"
        )

    frame = np.frombuffer(message, dtype=np.uint8, offset=header_size)
    frame = frame.reshape((height, stride_bytes))
    frame = frame[:, : expected_min_stride]
    frame = frame.reshape((height, width, channels))

    return RawFramePacket(
        frame_id=frame_id,
        capture_utc_us=capture_utc_us,
        send_utc_us=send_utc_us,
        width=width,
        height=height,
        channels=channels,
        stride_bytes=stride_bytes,
        payload_size=payload_size,
        pixel_format=pixel_format,
        image=frame,
    )


def parse_legacy_raw_frame(message: bytes) -> Optional[RawFramePacket]:
    if len(message) < 12:
        return None

    width, height, channels = struct.unpack_from("<iii", message, 0)
    payload = message[12:]
    expected = width * height * channels
    if width <= 0 or height <= 0 or channels not in (3, 4):
        return None
    if len(payload) != expected:
        return None

    frame = np.frombuffer(payload, dtype=np.uint8).reshape((height, width, channels))
    now_us = int(time.time() * 1_000_000)
    return RawFramePacket(
        frame_id=0,
        capture_utc_us=now_us,
        send_utc_us=now_us,
        width=width,
        height=height,
        channels=channels,
        stride_bytes=width * channels,
        payload_size=len(payload),
        pixel_format=RAW_PIXEL_FORMAT_BGRA8 if channels == 4 else 0,
        image=frame,
    )


def parse_xraw_frame(message: bytes) -> Optional[RawFramePacket]:
    if len(message) < XRAW_HEADER_SIZE:
        return None

    (
        magic,
        version,
        width,
        height,
        channels,
        frame_id,
        source_wall_time_ns,
        source_monotonic_ns,
    ) = struct.unpack_from(XRAW_HEADER_FMT, message, 0)

    if magic != XRAW_MAGIC or version != 1:
        return None

    payload = message[XRAW_HEADER_SIZE:]
    expected = width * height * channels
    if width <= 0 or height <= 0 or channels not in (3, 4):
        raise ValueError(f"invalid xraw shape: {width}x{height}x{channels}")
    if len(payload) != expected:
        raise ValueError(
            f"xraw payload size mismatch: expect {expected}, got {len(payload)}"
        )

    frame = np.frombuffer(payload, dtype=np.uint8).reshape((height, width, channels))
    capture_utc_us = int(source_wall_time_ns // 1000) if source_wall_time_ns > 0 else 0
    send_utc_us = capture_utc_us
    return RawFramePacket(
        frame_id=int(frame_id),
        capture_utc_us=capture_utc_us,
        send_utc_us=send_utc_us,
        width=int(width),
        height=int(height),
        channels=int(channels),
        stride_bytes=int(width * channels),
        payload_size=len(payload),
        pixel_format=RAW_PIXEL_FORMAT_BGRA8 if channels == 4 else 0,
        image=frame,
    )


def parse_encoded_packet(message: bytes) -> Optional[EncodedPacket]:
    if len(message) < ENC_OUTER_HEADER_SIZE + ENC_HEADER_SIZE:
        return None

    body_size = struct.unpack_from(">I", message, 0)[0]
    if body_size + ENC_OUTER_HEADER_SIZE != len(message):
        return None

    (
        magic,
        version,
        header_size,
        frame_id,
        capture_utc_us,
        send_utc_us,
        payload_size,
        _reserved,
    ) = struct.unpack_from(ENC_HEADER_FMT, message, ENC_OUTER_HEADER_SIZE)

    if magic != ENC_MAGIC or version != 2 or header_size != ENC_HEADER_SIZE:
        return None

    payload_offset = ENC_OUTER_HEADER_SIZE + header_size
    payload = message[payload_offset:]
    if len(payload) != payload_size:
        raise ValueError(
            f"encoded payload size mismatch: expect {payload_size}, got {len(payload)}"
        )

    return EncodedPacket(
        frame_id=frame_id,
        capture_utc_us=capture_utc_us,
        send_utc_us=send_utc_us,
        payload_size=payload_size,
        payload=payload,
    )


def to_bgr(raw: RawFramePacket) -> np.ndarray:
    if raw.channels == 4:
        return cv2.cvtColor(raw.image, cv2.COLOR_BGRA2BGR)
    return raw.image


class ZMQReceiver:
    def __init__(self, endpoint: str):
        self.context = zmq.Context()
        self.socket = self.context.socket(zmq.SUB)
        self.socket.connect(endpoint)
        self.socket.setsockopt(zmq.SUBSCRIBE, b"")
        self.poller = zmq.Poller()
        self.poller.register(self.socket, zmq.POLLIN)
        print(f"[receiver] connected to {endpoint}")

    def recv(self, timeout_ms: int = 100) -> Optional[bytes]:
        events = dict(self.poller.poll(timeout_ms))
        if self.socket not in events:
            return None
        return self.socket.recv()

    def close(self):
        self.socket.close()
        self.context.term()


def format_latency(now_us: int, ref_us: int) -> str:
    if ref_us <= 0:
        return "n/a"
    return f"{(now_us - ref_us) / 1000.0:.1f}ms"


def detect_message(message: bytes, mode: str) -> Tuple[str, object]:
    if mode in ("auto", "raw"):
        raw = parse_raw_frame(message)
        if raw is not None:
            return "raw", raw
        xraw = parse_xraw_frame(message)
        if xraw is not None:
            return "raw", xraw
        legacy = parse_legacy_raw_frame(message)
        if legacy is not None:
            return "raw", legacy

    if mode in ("auto", "encoded"):
        encoded = parse_encoded_packet(message)
        if encoded is not None:
            return "encoded", encoded

    raise ValueError("unrecognized packet format")


def run_raw_display(receiver: ZMQReceiver, mode: str):
    frame_count = 0
    last_fps_time = time.time()
    fps_counter = 0
    fps = 0.0

    print("[receiver] raw display mode, press q to quit, s to save")

    while True:
        message = receiver.recv(100)
        if message is None:
            key = cv2.waitKey(1) & 0xFF
            if key == ord("q"):
                break
            continue

        kind, packet = detect_message(message, mode)
        if kind != "raw":
            print("[receiver] got encoded packet while in raw display mode, skip")
            continue

        raw: RawFramePacket = packet
        bgr = to_bgr(raw)
        display = cv2.resize(bgr, (1280, 360))

        fps_counter += 1
        now = time.time()
        if now - last_fps_time >= 1.0:
            fps = fps_counter / (now - last_fps_time)
            fps_counter = 0
            last_fps_time = now

        now_us = int(time.time() * 1_000_000)
        info = (
            f"frame={raw.frame_id} {raw.width}x{raw.height}x{raw.channels} "
            f"fps={fps:.1f} cap_age={format_latency(now_us, raw.capture_utc_us)} "
            f"send_age={format_latency(now_us, raw.send_utc_us)}"
        )
        cv2.putText(
            display,
            info,
            (10, 30),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.7,
            (0, 255, 0),
            2,
        )
        cv2.imshow("ZMQ Raw Stream", display)

        key = cv2.waitKey(1) & 0xFF
        if key == ord("q"):
            break
        if key == ord("s"):
            filename = f"zmq_raw_{raw.frame_id:06d}.png"
            cv2.imwrite(filename, bgr)
            print(f"[receiver] saved {filename}")
        frame_count += 1

    print(f"[receiver] raw frames received: {frame_count}")


def run_encoded_monitor(receiver: ZMQReceiver, mode: str, save_path: Optional[str]):
    frame_count = 0
    writer = open(save_path, "ab") if save_path else None
    if writer:
        print(f"[receiver] encoded payload will be appended to {save_path}")

    try:
        while True:
            message = receiver.recv(500)
            if message is None:
                continue

            kind, packet = detect_message(message, mode)
            if kind != "encoded":
                print("[receiver] got raw packet while in encoded mode, skip")
                continue

            encoded: EncodedPacket = packet
            frame_count += 1
            if writer:
                writer.write(encoded.payload)
                writer.flush()

            if frame_count == 1 or frame_count % 30 == 0:
                now_us = int(time.time() * 1_000_000)
                print(
                    "[receiver] encoded"
                    f" frame={encoded.frame_id}"
                    f" payload={encoded.payload_size}"
                    f" cap_age={format_latency(now_us, encoded.capture_utc_us)}"
                    f" send_age={format_latency(now_us, encoded.send_utc_us)}"
                )
    except KeyboardInterrupt:
        pass
    finally:
        if writer:
            writer.close()
        print(f"[receiver] encoded frames received: {frame_count}")


def main():
    parser = argparse.ArgumentParser(description="XRoboToolkit webcam ZMQ receiver")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=5556)
    parser.add_argument(
        "--mode",
        choices=["auto", "raw", "encoded"],
        default="auto",
        help="auto/raw 用于 raw 显示；encoded 用于监控或保存编码流",
    )
    parser.add_argument(
        "--save-h264",
        default=None,
        help="encoded 模式下把 payload 追加保存到指定文件",
    )
    args = parser.parse_args()

    endpoint = f"tcp://{args.host}:{args.port}"
    receiver = ZMQReceiver(endpoint)
    try:
        if args.mode == "encoded":
            run_encoded_monitor(receiver, args.mode, args.save_h264)
        else:
            run_raw_display(receiver, args.mode)
    finally:
        receiver.close()
        cv2.destroyAllWindows()


if __name__ == "__main__":
    main()
