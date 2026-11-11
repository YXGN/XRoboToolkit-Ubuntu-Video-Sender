#!/usr/bin/env bash
set -euo pipefail

# JPEG variant of start_four_cams_new.sh. The three USB cameras remain one
# process each; only their XRAW payload encoding changes from raw BGR to JPEG.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SENDER_PY="${SCRIPT_DIR}/mjpg_zmq_sender.py"
SENDER_BIN="${SCRIPT_DIR}/OrinVideoSender"

BIND_IP="${BIND_IP:-192.168.123.164}"
HEAD_CAM="${HEAD_CAM:-/dev/video0}"
LEFT_WRIST_CAM="${LEFT_WRIST_CAM:-/dev/video4}"
RIGHT_WRIST_CAM="${RIGHT_WRIST_CAM:-/dev/video2}"
PICO_STEREO_CAM="${PICO_STEREO_CAM:-/dev/video6}"

HEAD_ZMQ_PORT="${HEAD_ZMQ_PORT:-5556}"
LEFT_WRIST_ZMQ_PORT="${LEFT_WRIST_ZMQ_PORT:-5557}"
RIGHT_WRIST_ZMQ_PORT="${RIGHT_WRIST_ZMQ_PORT:-5558}"
PICO_LISTEN_PORT="${PICO_LISTEN_PORT:-13579}"

FPS="${FPS:-60}"
WIDTH="${WIDTH:-640}"
HEIGHT="${HEIGHT:-480}"
JPEG_QUALITY="${JPEG_QUALITY:-80}"
ENABLE_PICO="${ENABLE_PICO:-1}"

if [[ ! -f "${SENDER_PY}" ]]; then
  echo "JPEG sender not found: ${SENDER_PY}" >&2
  exit 1
fi
if [[ "${ENABLE_PICO}" == "1" && ! -x "${SENDER_BIN}" ]]; then
  echo "PICO sender binary not found or not executable: ${SENDER_BIN}" >&2
  echo "Set ENABLE_PICO=0 for a three-camera JPEG-only test." >&2
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
  echo "${role} JPEG -> tcp://${BIND_IP}:${port}  cam=${device}  ${WIDTH}x${HEIGHT}@${FPS}"
  python "${SENDER_PY}" \
    --camera "${role}=${device}" \
    --bind "${role}=tcp://${BIND_IP}:${port}" \
    --width "${WIDTH}" \
    --height "${HEIGHT}" \
    --fps "${FPS}" \
    --encoding jpeg \
    --jpeg-quality "${JPEG_QUALITY}" &
  pids+=("$!")
}

start_camera head_fpv "${HEAD_CAM}" "${HEAD_ZMQ_PORT}"
start_camera left_hand "${LEFT_WRIST_CAM}" "${LEFT_WRIST_ZMQ_PORT}"
start_camera right_hand "${RIGHT_WRIST_CAM}" "${RIGHT_WRIST_ZMQ_PORT}"

if [[ "${ENABLE_PICO}" == "1" ]]; then
  echo "PICO stereo listen -> ${BIND_IP}:${PICO_LISTEN_PORT}  cam=${PICO_STEREO_CAM}  fps=${FPS}"
  "${SENDER_BIN}" \
    --listen "${BIND_IP}:${PICO_LISTEN_PORT}" \
    --stereo-camera "${PICO_STEREO_CAM}" \
    --width "${PICO_WIDTH:-2560}" \
    --height "${PICO_HEIGHT:-720}" \
    --fps "${FPS}" \
    --bitrate "${PICO_BITRATE:-20000000}" &
  pids+=("$!")
else
  echo "PICO disabled: ENABLE_PICO=${ENABLE_PICO}"
fi

echo
echo "JPEG camera sender processes started."
echo "  bind ip                  : ${BIND_IP}"
echo "  head jpeg endpoint       : tcp://${BIND_IP}:${HEAD_ZMQ_PORT}"
echo "  left wrist jpeg endpoint: tcp://${BIND_IP}:${LEFT_WRIST_ZMQ_PORT}"
echo "  right wrist jpeg endpoint: tcp://${BIND_IP}:${RIGHT_WRIST_ZMQ_PORT}"
echo "  fps / jpeg quality       : ${FPS} / ${JPEG_QUALITY}"
echo
echo "Press Ctrl+C to stop all."

wait
