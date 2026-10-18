#!/usr/bin/env bash
# Build the HCP app for a target board inside the env-zephyr-espressif image
# (one-shot container; output persists to host under hcp/build/<slug>).
#
# Usage:
#   ./build.sh                                  # default board, no LED
#   ./build.sh hcp_c6/esp32c6/hpcore --led      # + WS2812 (per-board overlay + lib/led conf)
#   ./build.sh <board> -- <extra west args>     # arbitrary extra cmake args
set -euo pipefail

BOARD="${1:-hcp_c6/esp32c6/hpcore}"; shift || true
IMAGE="env-zephyr-espressif"
APP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"   # .../zephyr/hcp

# Collect optional extra west build args; `--led` is sugar for the WS2812 opt-in,
# resolving the per-board DT overlay (boards/helios/<name>/<name>-ws2812.overlay).
BOARD_NAME="${BOARD%%/*}"
WS2812_OVERLAY="boards/helios/${BOARD_NAME}/${BOARD_NAME}-ws2812.overlay"
EXTRA_ARGS=""
while [ $# -gt 0 ]; do
  case "$1" in
    --led)
      if [ ! -f "${APP_DIR}/${WS2812_OVERLAY}" ]; then
        echo "no WS2812 overlay for board '${BOARD_NAME}': ${WS2812_OVERLAY}" >&2; exit 1
      fi
      EXTRA_ARGS="$EXTRA_ARGS -DEXTRA_CONF_FILE=lib/led/ws2812-spi.conf -DEXTRA_DTC_OVERLAY_FILE=${WS2812_OVERLAY}"
      WS2812=1
      shift ;;
    --)
      shift; EXTRA_ARGS="$EXTRA_ARGS $*"; break ;;
    *)
      EXTRA_ARGS="$EXTRA_ARGS $1"; shift ;;
  esac
done

WS_PARENT="$(dirname "${APP_DIR}")"                         # .../zephyr  (mounted as /workspace)
SLUG="$(echo "${BOARD}" | tr '/-' '__')"
if [ "${WS2812:-0}" = "1" ]; then SLUG="${SLUG}_led"; fi

exec docker run --rm \
  -v "${WS_PARENT}":/workspace \
  --entrypoint /bin/bash "${IMAGE}" -lc "
    set -e
    source /opt/venv/bin/activate
    source /opt/toolchains/zephyr/zephyr-env.sh
    export ZEPHYR_BASE=/opt/toolchains/zephyr
    cd /workspace/hcp
    west build -p auto -d /workspace/hcp/build/${SLUG} -b ${BOARD} . -- -DBOARD_ROOT=/workspace/hcp ${EXTRA_ARGS}
  "
