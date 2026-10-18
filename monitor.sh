#!/usr/bin/env bash
# monitor.sh — watch HCP serial logs from the host (macOS/Linux).
# The board is flashed from the host (Docker can't pass serial on macOS) and
# logs go over the native USB-Serial-JTAG CDC port (/dev/cu.usbmodem*).
#
# Usage:
#   ./monitor.sh                 # auto-detect port, open miniterm @115200
#   ./monitor.sh -r              # hard-reset the chip first, then attach
#   ./monitor.sh -p /dev/cu.usbmodem101 -b 115200
#   ./monitor.sh --raw           # non-interactive ~10s capture (for scripts/CI)
# miniterm exit: Ctrl-A then X.
set -euo pipefail

VENV="${VENV:-/Users/yabin/Documents/zephyr/venv}"
ESPTOOL="${ESPTOOL:-$VENV/bin/esptool}"
MINITERM="${MINITERM:-$VENV/bin/pyserial-miniterm}"
PYTHON="${PYTHON:-$VENV/bin/python}"
BAUD="${BAUD:-115200}"
CHIP="${CHIP:-esp32c6}"
RESET=0
RAW=0
PORT="${PORT:-}"

usage() {
  cat <<'EOF'
monitor.sh — watch HCP serial logs from the host (macOS/Linux).
Usage:
  ./monitor.sh                 auto-detect port, open miniterm @115200
  ./monitor.sh -r              hard-reset the chip first, then attach
  ./monitor.sh -p PORT -b BAUD explicit port/baud
  ./monitor.sh -c esp32c5      override chip used for --reset
  ./monitor.sh --raw           non-interactive ~10s capture
Env overrides: VENV, ESPTOOL, MINITERM, PYTHON, BAUD, CHIP, PORT.
miniterm exit: Ctrl-A then X.
EOF
}

while [ $# -gt 0 ]; do
  case "$1" in
    -p|--port)  PORT="$2"; shift 2 ;;
    -b|--baud)  BAUD="$2"; shift 2 ;;
    -c|--chip)  CHIP="$2"; shift 2 ;;
    -r|--reset) RESET=1; shift ;;
    --raw)      RAW=1; shift ;;
    -h|--help)  usage; exit 0 ;;
    *) echo "unknown arg: $1" >&2; usage; exit 1 ;;
  esac
done

if [ -z "$PORT" ]; then
  PORT=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1 || true)
  [ -z "$PORT" ] && PORT=$(ls /dev/ttyACM* 2>/dev/null | head -1 || true)
fi
if [ -z "$PORT" ]; then
  echo "No /dev/cu.usbmodem* (or ttyACM*) found. Is the board plugged in?" >&2
  exit 1
fi
echo "Using port: $PORT  (baud $BAUD, chip $CHIP)"

if [ "$RESET" -eq 1 ]; then
  echo "Resetting chip via esptool ($CHIP)..."
  "$ESPTOOL" --chip "$CHIP" -p "$PORT" --no-stub --after hard_reset chip_id \
      >/dev/null 2>&1 || { echo "esptool reset failed (port busy?)"; exit 1; }
  sleep 1
fi

if [ "$RAW" -eq 1 ]; then
  echo "Streaming ~10s of raw output (Ctrl-C to stop):"
  "$PYTHON" - "$PORT" "$BAUD" <<'PY'
import sys, time, serial
port, baud = sys.argv[1], int(sys.argv[2])
with serial.Serial(port, baud, timeout=1) as s:
    end = time.time() + 10
    while time.time() < end:
        line = s.readline()
        if line:
            sys.stdout.write(line.decode("utf-8", "replace")); sys.stdout.flush()
PY
  exit 0
fi

echo "Opening miniterm. Press Enter then Ctrl-A, X to exit. Tip: press the board's RST to see the boot banner."
exec "$MINITERM" "$PORT" "$BAUD"
