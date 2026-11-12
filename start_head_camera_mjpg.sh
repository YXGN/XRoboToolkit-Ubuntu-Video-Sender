#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"

exec python mjpg_zmq_sender.py \
  --camera head_fpv=/dev/video0 \
  --bind head_fpv=tcp://0.0.0.0:5556 \
  --width 640 \
  --height 480 \
  --fps 30 \
  --encoding jpeg \
  --jpeg-quality 80
