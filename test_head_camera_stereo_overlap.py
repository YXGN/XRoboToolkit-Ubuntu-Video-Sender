#!/usr/bin/env python3
"""
头部相机抓一帧 + SBS 左右切分与半透明重叠。

采集源（--source）:
  uvc    - 调用 test_stereo_capture（与 OrinVideoSender 相同 libuvc / UvcCameraSource）
  v4l2   - OpenCV V4L2（有 /dev/video* 时）
  sdk    - unitree_sdk2py VideoClient（Go2 内置相机；G1 通常不可用）

G1 头部 USB 双目（推荐）:
  make test-stereo
  python3 test_head_camera_stereo_overlap.py --source uvc --uvc-serial 01.00.00

或直接:
  ./test_stereo_capture --uvc-serial 01.00.00 --out capture
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import time
from pathlib import Path

import cv2
import numpy as np

_REPO = Path(__file__).resolve().parent
_TEST_STEREO_BIN = _REPO / "test_stereo_capture"

_SDK_ERR_SEND = 3102
_SDK_ERR_TIMEOUT = 3104


def stereo_split_overlap(bgr: np.ndarray, alpha: float = 0.5):
    mid = bgr.shape[1] // 2
    left = bgr[:, :mid].copy()
    right = bgr[:, mid : mid + left.shape[1]].copy()
    overlap = cv2.addWeighted(left, alpha, right, 1.0 - alpha, 0)
    return left, right, overlap


def save_outputs(bgr: np.ndarray, prefix: str, alpha: float) -> None:
    prefix_path = Path(prefix)
    prefix_path.parent.mkdir(parents=True, exist_ok=True)
    stem = prefix_path.stem if prefix_path.suffix else str(prefix_path)
    parent = prefix_path.parent if prefix_path.suffix else prefix_path.parent
    base = parent / stem

    cv2.imwrite(str(base) + "_full.jpg", bgr)

    if bgr.shape[1] >= 2 * bgr.shape[0]:
        left, right, overlap = stereo_split_overlap(bgr, alpha)
        cv2.imwrite(str(base) + "_left.jpg", left)
        cv2.imwrite(str(base) + "_right.jpg", right)
        cv2.imwrite(str(base) + "_overlap.jpg", overlap)
        print(
            f"stereo SBS {bgr.shape[1]}x{bgr.shape[0]} -> "
            f"eye {left.shape[1]}x{left.shape[0]}"
        )
    else:
        print(
            f"mono or non-SBS ({bgr.shape[1]}x{bgr.shape[0]}): "
            "only saved *_full.jpg"
        )


def _sdk_error_hint(code: int) -> str:
    if code == _SDK_ERR_SEND:
        return (
            f"code={code}: DDS RPC 失败。G1 请用:\n"
            "  make test-stereo && "
            "python3 test_head_camera_stereo_overlap.py --source uvc --uvc-serial 01.00.00"
        )
    if code == _SDK_ERR_TIMEOUT:
        return f"code={code}: 请求超时，检查网卡 iface。"
    return f"code={code}"


def capture_via_video_client(iface: str, timeout_s: float = 3.0, retries: int = 5):
    from unitree_sdk2py.core.channel import ChannelFactoryInitialize
    from unitree_sdk2py.go2.video.video_client import VideoClient

    ChannelFactoryInitialize(0, iface)
    client = VideoClient()
    client.SetTimeout(timeout_s)
    client.Init()

    last_code = -1
    for attempt in range(1, retries + 1):
        code, data = client.GetImageSample()
        last_code = code
        if code == 0 and data:
            buf = np.frombuffer(data, dtype=np.uint8)
            frame = cv2.imdecode(buf, cv2.IMREAD_COLOR)
            if frame is not None and frame.size > 0:
                return frame
            print(f"[warn] attempt {attempt}: JPEG decode failed", file=sys.stderr)
        else:
            print(
                f"[warn] attempt {attempt}: GetImageSample code={code}, "
                f"bytes={len(data) if data else 0}",
                file=sys.stderr,
            )
        time.sleep(0.3)

    raise RuntimeError(
        f"GetImageSample failed after {retries} tries.\n{_sdk_error_hint(last_code)}"
    )


def capture_via_sender_libuvc(
    uid: str = "",
    serial: str = "",
    fps: int = 60,
    out_prefix: str = "capture",
    alpha: float = 0.5,
) -> None:
    """
    与 OrinVideoSender 相同：C++ UvcCameraSource（libuvc），不用 Python pyuvc。
    pyuvc 与 libuvc 是不同栈，常出现 available_modes 有但 isochronous 启动失败。
    """
    exe = _TEST_STEREO_BIN
    if not exe.is_file():
        raise RuntimeError(
            f"未找到 {exe}，请先编译:\n"
            "  cd ~/XRoboToolkit-Ubuntu-Video-Sender && make test-stereo"
        )

    cmd = [str(exe), "--out", out_prefix, "--fps", str(fps), "--alpha", str(alpha)]
    if uid:
        cmd.extend(["--uvc-uid", uid])
    if serial:
        cmd.extend(["--uvc-serial", serial])

    print(f"[libuvc] {' '.join(cmd)}")
    subprocess.run(cmd, check=True)


def capture_via_v4l2(
    device: str = "/dev/video0",
    width: int = 2560,
    height: int = 720,
    fps: int = 60,
    warmup: int = 5,
):
    print(f"[v4l2] open {device} {width}x{height}@{fps} MJPG")
    cap = cv2.VideoCapture(device, cv2.CAP_V4L2)
    if not cap.isOpened():
        raise RuntimeError(f"无法打开 {device}（CAP_V4L2）")

    cap.set(cv2.CAP_PROP_FOURCC, cv2.VideoWriter_fourcc(*"MJPG"))
    cap.set(cv2.CAP_PROP_FRAME_WIDTH, width)
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, height)
    cap.set(cv2.CAP_PROP_FPS, fps)

    for i in range(warmup):
        ok, frame = cap.read()
        if ok and frame is not None and frame.size > 0:
            if i + 1 == warmup:
                cap.release()
                return frame
        time.sleep(0.05)

    cap.release()
    raise RuntimeError(f"v4l2 从 {device} 读帧失败")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Head camera snapshot + stereo overlap"
    )
    parser.add_argument(
        "iface",
        nargs="?",
        default="",
        help="--source sdk 时必填网卡名",
    )
    parser.add_argument(
        "--source",
        choices=("uvc", "v4l2", "sdk"),
        default="uvc",
        help="采集方式（默认 uvc = OrinVideoSender 同款 libuvc）",
    )
    parser.add_argument("--out", default="capture", help="输出前缀")
    parser.add_argument("--alpha", type=float, default=0.5)
    parser.add_argument("--timeout", type=float, default=3.0)
    parser.add_argument("--retries", type=int, default=5)
    parser.add_argument("--uvc-uid", default="")
    parser.add_argument("--uvc-serial", default="")
    parser.add_argument("--device", default="/dev/video0")
    parser.add_argument("--width", type=int, default=2560)
    parser.add_argument("--height", type=int, default=720)
    parser.add_argument("--fps", type=int, default=60)
    args = parser.parse_args()

    if not 0.0 <= args.alpha <= 1.0:
        print("error: --alpha must be in [0, 1]", file=sys.stderr)
        return 2

    print(f"source={args.source}")

    if args.source == "uvc":
        capture_via_sender_libuvc(
            uid=args.uvc_uid,
            serial=args.uvc_serial,
            fps=args.fps,
            out_prefix=args.out,
            alpha=args.alpha,
        )
        print(f"saved under prefix: {args.out}")
        return 0

    if args.source == "sdk":
        if not args.iface:
            print("error: --source sdk 需要网卡，如: ... --source sdk eth0", file=sys.stderr)
            return 2
        bgr = capture_via_video_client(args.iface, args.timeout, args.retries)
    else:
        bgr = capture_via_v4l2(
            device=args.device,
            width=args.width,
            height=args.height,
            fps=args.fps,
        )

    print(f"decoded frame: {bgr.shape[1]}x{bgr.shape[0]}")
    save_outputs(bgr, args.out, args.alpha)
    print(f"saved under prefix: {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
