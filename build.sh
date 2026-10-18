#!/usr/bin/env bash
# Build the HCP app for a target board inside the env-zephyr-espressif image
# (one-shot container; output persists to host under hcp/build/<slug>).
#
# WS2812 (addressable LED) is ON by default for any board that ships a
# boards/helios/<board>/<board>-ws2812.overlay. Opt out with --no-led.
#
# Usage:
#   ./build.sh                                  # default board (hcp_c6), LED on
#   ./build.sh all                              # build hcp_c3 + hcp_c5 + hcp_c6
#   ./build.sh hcp_c3                           # one board, LED on
#   ./build.sh hcp_c6/esp32c6/hpcore --no-led   # LED off
#   ./build.sh <board> -- <extra west args>     # arbitrary extra cmake args
set -euo pipefail

IMAGE="env-zephyr-espressif"
APP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"   # .../zephyr/hcp

# ---- parse args ----
BOARD_OR_ALL="hcp_c6/esp32c6/hpcore"
WANT_LED=1                 # default ON
EXTRA_ARGS=""
positional_seen=0
while [ $# -gt 0 ]; do
  case "$1" in
    all)               BOARD_OR_ALL="all"; positional_seen=1; shift ;;
    --no-led)          WANT_LED=0; shift ;;
    --led)             WANT_LED=1; shift ;;          # explicit (already default)
    --)                shift; EXTRA_ARGS="$EXTRA_ARGS $*"; break ;;
    -*)                echo "unknown option: $1" >&2; exit 1 ;;
    *)                 if [ "$positional_seen" -eq 0 ]; then BOARD_OR_ALL="$1"; positional_seen=1; shift;
                       else EXTRA_ARGS="$EXTRA_ARGS $1"; shift; fi ;;
  esac
done

# ---- build one board ----
build_one() {
  local BOARD="$1"
  local BOARD_NAME="${BOARD%%/*}"
  local OVERLAY="boards/helios/${BOARD_NAME}/${BOARD_NAME}-ws2812.overlay"
  local LED_ARGS="" USE_LED="$WANT_LED"

  if [ "$USE_LED" = "1" ]; then
    if [ -f "${APP_DIR}/${OVERLAY}" ]; then
      LED_ARGS="-DEXTRA_CONF_FILE=lib/led/ws2812-spi.conf -DEXTRA_DTC_OVERLAY_FILE=${OVERLAY}"
    else
      echo "note: no ws2812 overlay for '${BOARD_NAME}' (${OVERLAY}); building without LED" >&2
      LED_ARGS=""; USE_LED=0
    fi
  fi

  local SLUG; SLUG="$(echo "${BOARD}" | tr '/-' '__')"
  [ "$USE_LED" = "1" ] && SLUG="${SLUG}_led"

  echo ">>> building ${BOARD}  (LED=${USE_LED})  -> build/${SLUG}"
  docker run --rm \
    -v "$(dirname "${APP_DIR}")":/workspace \
    --entrypoint /bin/bash "${IMAGE}" -lc "
      set -e
      source /opt/venv/bin/activate
      source /opt/toolchains/zephyr/zephyr-env.sh
      export ZEPHYR_BASE=/opt/toolchains/zephyr
      cd /workspace/hcp
      west build -p auto -d /workspace/hcp/build/${SLUG} -b ${BOARD} . -- \
        -DBOARD_ROOT=/workspace/hcp ${LED_ARGS} ${EXTRA_ARGS}
    "
}

# ---- dispatch ----
if [ "$BOARD_OR_ALL" = "all" ]; then
  for b in hcp_c5/esp32c5/hpcore hcp_c6/esp32c6/hpcore hcp_c3; do
    build_one "$b"
  done
else
  build_one "$BOARD_OR_ALL"
fi
