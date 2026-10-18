#!/usr/bin/env bash
# Build the HCP app for a target board inside the env-zephyr-espressif image
# (one-shot container; output persists to host under hcp/build/<slug>).
#
# Usage:
#   ./build.sh                                  # default board, no LED
#   ./build.sh hcp_c6/esp32c6/hpcore --led      # + WS2812 (lib/led overlay+conf)
#   ./build.sh <board> -- <extra west args>     # arbitrary extra cmake args
set -euo pipefail

BOARD="${1:-hcp_c6/esp32c6/hpcore}"; shift || true
IMAGE="env-zephyr-espressif"

# Collect optional extra west build args; `--led` is sugar for the WS2812 opt-in.
EXTRA_ARGS=""
while [ $# -gt 0 ]; do
  case "$1" in
    --led)
      EXTRA_ARGS="$EXTRA_ARGS -DEXTRA_CONF_FILE=lib/led/ws2812-spi.conf -DEXTRA_DTC_OVERLAY_FILE=lib/led/ws2812-spi.overlay"
      shift ;;
    --)
      shift; EXTRA_ARGS="$EXTRA_ARGS $*"; break ;;
    *)
      EXTRA_ARGS="$EXTRA_ARGS $1"; shift ;;
  esac
done

APP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"   # .../zephyr/hcp
WS_PARENT="$(dirname "${APP_DIR}")"                         # .../zephyr  (mounted as /workspace)
SLUG="$(echo "${BOARD}" | tr '/-' '__')"

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
