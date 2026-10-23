#!/usr/bin/env bash
# Build the HCP app for a target board inside the env-zephyr-espressif image
# (one-shot container; output persists to host under hcp/build/<slug>).
#
# The WS2812 LED is described in each board's devicetree and driven by the app
# by default (CONFIG_HCP_LED=y). No extra overlay args are needed. Opt out with
# --no-led (also drops the led_strip/SPI drivers from the binary).
#
# Usage:
#   ./build.sh                       # default board (hcp_c6), LED on
#   ./build.sh all                   # build hcp_c3 + hcp_c5 + hcp_c6
#   ./build.sh hcp_c3                # one board, LED on
#   ./build.sh <board> --no-led      # disable LED (Kconfig off)
#   ./build.sh <board> --rs485       # RS485 uart1 variant (per-board overlay)
#   ./build.sh <board> --version 0.2.0
#                                  # stamp this build as 0.2.0: both the version
#                                  # the application reports and the version
#                                  # imgtool writes into the MCUboot image header.
#                                  # Goes to its own build/<slug>_version dir, and
#                                  # zephyr/zephyr.signed.bin is what an update
#                                  # server should serve.
#   ./build.sh <board> -- <extra west args>
#                                  # also gets a directory of its own (build.sh
#                                  # fingerprints the arguments into the name), so a
#                                  # following plain build cannot inherit them.
set -euo pipefail

IMAGE="env-zephyr-espressif"
APP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"   # .../zephyr/hcp

BOARD_OR_ALL="hcp_c6/esp32c6/hpcore"
WANT_LED=1
WANT_RS485=0
VERSION=""
EXTRA_ARGS=""
positional_seen=0
while [ $# -gt 0 ]; do
  case "$1" in
    all)        BOARD_OR_ALL="all"; positional_seen=1; shift ;;
    --no-led)   WANT_LED=0; shift ;;
    --led)      WANT_LED=1; shift ;;
    --rs485)    WANT_RS485=1; shift ;;
    --version)  VERSION="$2"; shift 2 ;;
    --)         shift; EXTRA_ARGS="$EXTRA_ARGS $*"; break ;;
    -*)         echo "unknown option: $1" >&2; exit 1 ;;
    *)          if [ "$positional_seen" -eq 0 ]; then BOARD_OR_ALL="$1"; positional_seen=1; shift;
                else EXTRA_ARGS="$EXTRA_ARGS $1"; shift; fi ;;
  esac
done

# LED-off is a pure Kconfig choice now (DT always describes the hardware).
LED_ARGS=""
if [ "$WANT_LED" = "0" ]; then
  LED_ARGS="-DCONFIG_HCP_LED=n -DCONFIG_LED_STRIP=n -DCONFIG_WS2812_STRIP_SPI=n"
fi

build_one() {
  local BOARD="$1"
  local BOARD_NAME="${BOARD%%/*}"
  local RS485_ARGS=""
  if [ "$WANT_RS485" = "1" ]; then
    local OV="boards/helios/${BOARD_NAME}/${BOARD_NAME}-rs485.overlay"
    if [ ! -f "${APP_DIR}/${OV}" ]; then
      echo "no rs485 overlay for '${BOARD_NAME}' (${OV})" >&2; exit 1
    fi
    RS485_ARGS="-DEXTRA_DTC_OVERLAY_FILE=${OV}"
  fi

  local SLUG; SLUG="$(echo "${BOARD}" | tr '/-' '__')"
  [ "$WANT_LED" = "0" ] && SLUG="${SLUG}_nold"
  [ "$WANT_RS485" = "1" ] && SLUG="${SLUG}_rs485"

  # A versioned build is a build of its own: the two version symbols have to change
  # together (an image whose MCUboot header still says 0.0.0+0 makes the
  # bootloader's downgrade prevention compare nothing, while the application keeps
  # reporting CONFIG_APP_VERSION), and a fresh directory avoids inheriting a
  # configured-from-elsewhere cache.
  local VERSION_ARGS=""
  if [ -n "$VERSION" ]; then
    SLUG="${SLUG}_v${VERSION}"
    mkdir -p "${APP_DIR}/build"
    printf 'CONFIG_APP_VERSION="%s"\nCONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION="%s"\n' \
      "$VERSION" "$VERSION" > "${APP_DIR}/build/.version.conf"
    VERSION_ARGS="-DEXTRA_CONF_FILE=/workspace/hcp/build/.version.conf"
  fi

  # Extra west/cmake arguments are part of the directory name too: west caches the
  # -D values it was configured with, so a plain ./build.sh after a customized one
  # would silently rebuild with the *previous* overrides still in .config (this bit
  # me once: a 20 s watchdog test image came back out of the "default" directory).
  if [ -n "$EXTRA_ARGS" ]; then
    SLUG="${SLUG}_x$(printf '%s' "$EXTRA_ARGS" | cksum | cut -d' ' -f1)"
  fi

  echo ">>> building ${BOARD}  (LED=${WANT_LED} RS485=${WANT_RS485}${VERSION:+ version=${VERSION}})  -> build/${SLUG}"
  docker run --rm \
    -v "$(dirname "${APP_DIR}")":/workspace \
    --entrypoint /bin/bash "${IMAGE}" -lc "
      set -e
      source /opt/venv/bin/activate
      source /opt/toolchains/zephyr/zephyr-env.sh
      export ZEPHYR_BASE=/opt/toolchains/zephyr
      cd /workspace/hcp
      west build -p auto -d /workspace/hcp/build/${SLUG} -b ${BOARD} . -- \
        -DBOARD_ROOT=/workspace/hcp ${LED_ARGS} ${RS485_ARGS} ${VERSION_ARGS} ${EXTRA_ARGS}
    "
}

if [ "$BOARD_OR_ALL" = "all" ]; then
  for b in hcp_c5/esp32c5/hpcore hcp_c6/esp32c6/hpcore hcp_c3; do
    build_one "$b"
  done
else
  build_one "$BOARD_OR_ALL"
fi
