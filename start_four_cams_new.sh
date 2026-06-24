#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SENDER_BIN="${SCRIPT_DIR}/OrinVideoSender"

BIND_IP="${BIND_IP:-192.168.123.164}"
HEAD_CAM="${HEAD_CAM:-/dev/video0}"
LEFT_WRIST_CAM="${LEFT_WRIST_CAM:-/dev/video4}"
RIGHT_WRIST_CAM="${RIGHT_WRIST_CAM:-/dev/video2}"
PICO_STEREO_CAM="${PICO_STEREO_CAM:-/dev/video0}"

HEAD_ZMQ_PORT="${HEAD_ZMQ_PORT:-5556}"
LEFT_WRIST_ZMQ_PORT="${LEFT_WRIST_ZMQ_PORT:-5557}"
RIGHT_WRIST_ZMQ_PORT="${RIGHT_WRIST_ZMQ_PORT:-5558}"
PICO_LISTEN_PORT="${PICO_LISTEN_PORT:-13579}"

FPS="${FPS:-30}"
PICO_WIDTH="${PICO_WIDTH:-2560}"
PICO_HEIGHT="${PICO_HEIGHT:-720}"
PICO_BITRATE="${PICO_BITRATE:-20000000}"

if [[ ! -x "${SENDER_BIN}" ]]; then
  echo "Sender binary not found or not executable: ${SENDER_BIN}" >&2
  echo "Please run: make" >&2
  exit 1
fi

cleanup() {
  trap - EXIT INT TERM
  jobs -pr | xargs -r kill
  wait || true
}
trap cleanup EXIT INT TERM

echo "[1/4] head raw -> tcp://${BIND_IP}:${HEAD_ZMQ_PORT}  cam=${HEAD_CAM}"
"${SENDER_BIN}" \
  --send \
  --zmq-raw "tcp://${BIND_IP}:${HEAD_ZMQ_PORT}" \
  --camera "${HEAD_CAM}" \
  --fps "${FPS}" &

echo "[2/4] left wrist raw -> tcp://${BIND_IP}:${LEFT_WRIST_ZMQ_PORT}  cam=${LEFT_WRIST_CAM}"
"${SENDER_BIN}" \
  --send \
  --zmq-raw "tcp://${BIND_IP}:${LEFT_WRIST_ZMQ_PORT}" \
  --camera "${LEFT_WRIST_CAM}" \
  --fps "${FPS}" &

echo "[3/4] right wrist raw -> tcp://${BIND_IP}:${RIGHT_WRIST_ZMQ_PORT}  cam=${RIGHT_WRIST_CAM}"
"${SENDER_BIN}" \
  --send \
  --zmq-raw "tcp://${BIND_IP}:${RIGHT_WRIST_ZMQ_PORT}" \
  --camera "${RIGHT_WRIST_CAM}" \
  --fps "${FPS}" &

echo "[4/4] pico stereo listen -> ${BIND_IP}:${PICO_LISTEN_PORT}  cam=${PICO_STEREO_CAM}"
"${SENDER_BIN}" \
  --listen "${BIND_IP}:${PICO_LISTEN_PORT}" \
  --stereo-camera "${PICO_STEREO_CAM}" \
  --width "${PICO_WIDTH}" \
  --height "${PICO_HEIGHT}" \
  --fps "${FPS}" \
  --bitrate "${PICO_BITRATE}" &

echo
echo "All 4 sender processes started."
echo "  bind ip                 : ${BIND_IP}"
echo "  head raw endpoint       : tcp://${BIND_IP}:${HEAD_ZMQ_PORT}"
echo "  left wrist raw endpoint : tcp://${BIND_IP}:${LEFT_WRIST_ZMQ_PORT}"
echo "  right wrist raw endpoint: tcp://${BIND_IP}:${RIGHT_WRIST_ZMQ_PORT}"
echo "  pico listen endpoint    : ${BIND_IP}:${PICO_LISTEN_PORT}"
echo
echo "Press Ctrl+C to stop all."

wait
