#!/usr/bin/env bash
set -euo pipefail

# Publish the head camera as the native 1280x480 SBS frame at 60 FPS.
# Wrist cameras remain independent 640x480 streams.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SENDER_PY="${SCRIPT_DIR}/mjpg_zmq_sender.py"

BIND_IP="${BIND_IP:-192.168.123.164}"
HEAD_CAM="${HEAD_CAM:-/dev/video0}"
LEFT_WRIST_CAM="${LEFT_WRIST_CAM:-/dev/video4}"
RIGHT_WRIST_CAM="${RIGHT_WRIST_CAM:-/dev/video2}"

HEAD_ZMQ_PORT="${HEAD_ZMQ_PORT:-5556}"
LEFT_WRIST_ZMQ_PORT="${LEFT_WRIST_ZMQ_PORT:-5557}"
RIGHT_WRIST_ZMQ_PORT="${RIGHT_WRIST_ZMQ_PORT:-5558}"

HEAD_WIDTH="${HEAD_WIDTH:-1280}"
HEAD_HEIGHT="${HEAD_HEIGHT:-480}"
HEAD_FPS="${HEAD_FPS:-60}"
WRIST_WIDTH="${WRIST_WIDTH:-640}"
WRIST_HEIGHT="${WRIST_HEIGHT:-480}"
WRIST_FPS="${WRIST_FPS:-60}"
JPEG_QUALITY="${JPEG_QUALITY:-80}"

if [[ ! -f "${SENDER_PY}" ]]; then
  echo "JPEG sender not found: ${SENDER_PY}" >&2
  exit 1
fi

if [[ -f "${HOME}/miniconda3/etc/profile.d/conda.sh" ]]; then
  # shellcheck disable=SC1090
  source "${HOME}/miniconda3/etc/profile.d/conda.sh"
  conda activate "${CONDA_ENV:-tv}"
fi

pids=()
cleanup() {
  trap - EXIT INT TERM
  if ((${#pids[@]})); then
    kill "${pids[@]}" 2>/dev/null || true
    wait "${pids[@]}" 2>/dev/null || true
  fi
}
trap cleanup EXIT INT TERM

start_camera() {
  local role="$1"
  local device="$2"
  local port="$3"
  local width="$4"
  local height="$5"
  local fps="$6"
  echo "${role} JPEG -> tcp://${BIND_IP}:${port} cam=${device} ${width}x${height}@${fps}"
  python "${SENDER_PY}" \
    --camera "${role}=${device}" \
    --bind "${role}=tcp://${BIND_IP}:${port}" \
    --width "${width}" \
    --height "${height}" \
    --fps "${fps}" \
    --encoding jpeg \
    --jpeg-quality "${JPEG_QUALITY}" &
  pids+=("$!")
}

start_camera head_fpv "${HEAD_CAM}" "${HEAD_ZMQ_PORT}" "${HEAD_WIDTH}" "${HEAD_HEIGHT}" "${HEAD_FPS}"
start_camera left_hand "${LEFT_WRIST_CAM}" "${LEFT_WRIST_ZMQ_PORT}" "${WRIST_WIDTH}" "${WRIST_HEIGHT}" "${WRIST_FPS}"
start_camera right_hand "${RIGHT_WRIST_CAM}" "${RIGHT_WRIST_ZMQ_PORT}" "${WRIST_WIDTH}" "${WRIST_HEIGHT}" "${WRIST_FPS}"

echo
echo "SBS head and wrist JPEG camera sender processes started."
echo "  head endpoint: tcp://${BIND_IP}:${HEAD_ZMQ_PORT} (${HEAD_WIDTH}x${HEAD_HEIGHT}@${HEAD_FPS})"
echo "  left endpoint: tcp://${BIND_IP}:${LEFT_WRIST_ZMQ_PORT} (${WRIST_WIDTH}x${WRIST_HEIGHT}@${WRIST_FPS})"
echo "  right endpoint: tcp://${BIND_IP}:${RIGHT_WRIST_ZMQ_PORT} (${WRIST_WIDTH}x${WRIST_HEIGHT}@${WRIST_FPS})"
echo "PICO stereo sender is intentionally not started by this VelaBot-only script."
echo
echo "Press Ctrl+C to stop all."

wait
