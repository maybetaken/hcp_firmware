#!/usr/bin/env bash
# Build the HCP app for a target board inside the env-zephyr-espressif image
# (one-shot container; output persists to host under hcp/build/<slug>).
#
# The WS2812 LED is described in each board's devicetree and driven by the app
# by default (CONFIG_HCP_LED=y). No extra overlay args are needed. Opt out with
# --no-led (also drops the led_strip driver itself from the binary).
#
# Usage:
#   ./build.sh                       # default board (hcp_c6), LED on
#   ./build.sh all                   # build hcp_c3 + hcp_c5 + hcp_c6
#   ./build.sh hcp_c3                # one board, LED on
#   ./build.sh <board> --no-led      # disable LED (Kconfig off)
#   ./build.sh <board> --bench       # bench/HIL image: LED cycle + Modbus poll,
#                                    # provisioning off (see CONFIG_HCP_BENCH)
#   ./build.sh <board> --rs485       # RS485 uart1 variant (per-board overlay)
#   ./build.sh <board> --version 2.0.3.14
#                                  # stamp this build as 2.0.3.14: both the version
#                                  # the application reports and the version
#                                  # imgtool writes into the MCUboot image header
#                                  # (spelled 2.0.3+14 there - see the note below).
#                                  # Goes to its own build/<slug>_version dir, and
#                                  # zephyr/zephyr.signed.bin is what an update
#                                  # server should serve.
#   ./build.sh <board> --signed    # sign the image with hcp/keys/$HCP_SIGN_KEY.pem
#                                  # (default root-ec256), i.e. produce an image an
#                                  # ECDSA-enabled MCUboot will boot. Must match how
#                                  # ./tools/build_mcuboot.sh --signed was built.
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
WANT_BENCH=0
VERSION=""
# Empty, not 0: the summary line below uses ${SIGNED:+ ...}, which triggers on any
# non-empty value.
SIGNED=""
HCP_SIGN_KEY="${HCP_SIGN_KEY:-root-ec256}"
EXTRA_ARGS=""
positional_seen=0
while [ $# -gt 0 ]; do
  case "$1" in
    all)        BOARD_OR_ALL="all"; positional_seen=1; shift ;;
    --no-led)   WANT_LED=0; shift ;;
    --led)      WANT_LED=1; shift ;;
    --rs485)  WANT_RS485=1; shift ;;
    --bench)  WANT_BENCH=1; shift ;;
    --version)  VERSION="$2"; shift 2 ;;
    --signed)   SIGNED=1; shift ;;
    --)         shift; EXTRA_ARGS="$EXTRA_ARGS $*"; break ;;
    -*)         echo "unknown option: $1" >&2; exit 1 ;;
    *)          if [ "$positional_seen" -eq 0 ]; then BOARD_OR_ALL="$1"; positional_seen=1; shift;
                else EXTRA_ARGS="$EXTRA_ARGS $1"; shift; fi ;;
  esac
done

# LED-off is a pure Kconfig choice now (DT always describes the hardware).
LED_ARGS=""
if [ "$WANT_LED" = "0" ]; then
  LED_ARGS="-DCONFIG_HCP_LED=n -DCONFIG_LED_STRIP=n -DCONFIG_LED_STRIP_ESP_RMT=n"
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

  # The bench image is a build of its own directory rather than a few extra -D
  # flags on the product one, because .config differs in ways worth keeping apart:
  # the aid threads are in, and device-domain logging is debug.
  #
  # It keeps hcp::provision ON deliberately. A bench device that is on the network
  # can be upgraded with ./tools/ota_release.sh and watched with
  # ./tools/mqtt_watch.py, so nothing has to be plugged into USB again; drop that
  # one benefit and the image stops being useful after the first release.
  local BENCH_ARGS=""
  if [ "$WANT_BENCH" = "1" ]; then
    BENCH_ARGS="-DCONFIG_HCP_BENCH=y -DCONFIG_HCP_LOG_LEVEL_DEVICE=4"
    SLUG="${SLUG}_bench"
  fi

  # A versioned build is a build of its own: the two version symbols have to change
  # together (an image whose MCUboot header still says 0.0.0+0 makes the
  # bootloader's downgrade prevention compare nothing, while the application keeps
  # reporting CONFIG_APP_VERSION), and a fresh directory avoids inheriting a
  # configured-from-elsewhere cache.
  #
  # Both overrides are written to one conf fragment rather than passed as
  # -DCONFIG_X=...: a string symbol surviving docker/bash/cmake loses its quotes and
  # Kconfig then refuses it as a "malformed string literal". The fragment is named
  # after the build directory on purpose - a single shared file would let the next
  # build change what a previous build reconfigures from.
  local confname=""
  if [ -n "$VERSION" ]; then
    confname="v${VERSION}"
    SLUG="${SLUG}_v${VERSION}"
  fi

  # Relative key path, which is what Zephyr resolves the signing key against
  # (APPLICATION_CONFIG_DIR, cmake/mcuboot.cmake); a missing file fails the build
  # with a clear message, the right answer to "signed image requested, no key".
  #
  # MCUBOOT_GENERATE_UNSIGNED_IMAGE has to be switched off explicitly: it is y
  # whenever no key is known, and CONFIG_MCUBOOT_SIGNATURE_KEY_FILE depends on it
  # being n - without this line the key assignment is silently dropped and the image
  # comes out with a hash TLV but no signature (Zephyr's hawkbit sample does the
  # same thing).
  if [ "$SIGNED" = "1" ]; then
    confname="${confname:+${confname}_}signed-${HCP_SIGN_KEY}"
    SLUG="${SLUG}_signed-${HCP_SIGN_KEY}"
  fi

  # Bench/factory pre-provisioning: an untracked wificreds.conf next to prj.conf is
  # folded into the same fragment - a second -DEXTRA_CONF_FILE on the command line
  # would simply override the first one, not add to it. The file carries a Wi-Fi
  # passphrase, so it is gitignored, and a build that contains it says so in its
  # directory name rather than looking like the default image.
  local WANT_WIFI_FRAG=0
  if [ -f "${APP_DIR}/wificreds.conf" ]; then
    confname="${confname:+${confname}_}wifi"
    SLUG="${SLUG}_wifi"
    WANT_WIFI_FRAG=1
  fi

  local VERSION_ARGS=""
  if [ -n "$confname" ]; then
    local FRAG="${APP_DIR}/build/.config-${confname}.conf"
    mkdir -p "${APP_DIR}/build"
    : > "$FRAG"
    if [ -n "$VERSION" ]; then
      # The application reports the version as written; the MCUboot image header has
      # to be told the same number in the only spelling imgtool accepts. Its regex
      # (scripts/imgtool/version.py in the MCUboot tree: ^maj(.min(.rev(+build)?)?)?$)
      # rejects a
      # dotted fourth field, and the header really does carry one - major(B) minor(B)
      # revision(H) build(I) - so "2.0.3.14" has to arrive as "2.0.3+14". Without
      # this a four-part --version fails the build inside imgtool with
      # "Invalid version number, should be maj.min.rev+build".
      local IMGTOOL_VERSION="$VERSION"
      if [[ "$VERSION" =~ ^([0-9]+\.[0-9]+\.[0-9]+)\.([0-9]+)$ ]]; then
        IMGTOOL_VERSION="${BASH_REMATCH[1]}+${BASH_REMATCH[2]}"
      fi
      printf 'CONFIG_APP_VERSION="%s"\nCONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION="%s"\n' \
        "$VERSION" "$IMGTOOL_VERSION" >> "$FRAG"
    fi
    if [ "$SIGNED" = "1" ]; then
      printf 'CONFIG_MCUBOOT_GENERATE_UNSIGNED_IMAGE=n\nCONFIG_MCUBOOT_SIGNATURE_KEY_FILE="keys/%s.pem"\n' \
        "$HCP_SIGN_KEY" >> "$FRAG"
    fi
    if [ "$WANT_WIFI_FRAG" = "1" ]; then
      cat "${APP_DIR}/wificreds.conf" >> "$FRAG"
      echo "    pre-provisioning credentials from wificreds.conf (bench/factory image)"
    fi
    VERSION_ARGS="-DEXTRA_CONF_FILE='/workspace/hcp/build/.config-${confname}.conf'"
  fi

  # Extra west/cmake arguments are part of the directory name too: west caches the
  # -D values it was configured with, so a plain ./build.sh after a customized one
  # would silently rebuild with the *previous* overrides still in .config (this bit
  # me once: a 20 s-watchdog test image came back out of the "default" directory).
  if [ -n "$EXTRA_ARGS" ]; then
    SLUG="${SLUG}_x$(printf '%s' "$EXTRA_ARGS" | cksum | cut -d' ' -f1)"
  fi

  echo ">>> building ${BOARD}  (LED=${WANT_LED} RS485=${WANT_RS485} BENCH=${WANT_BENCH}${VERSION:+ version=${VERSION}}${SIGNED:+ signed=${HCP_SIGN_KEY}})  -> build/${SLUG}"
  docker run --rm \
    -v "$(dirname "${APP_DIR}")":/workspace \
    --entrypoint /bin/bash "${IMAGE}" -lc "
      set -e
      source /opt/venv/bin/activate
      source /opt/toolchains/zephyr/zephyr-env.sh
      export ZEPHYR_BASE=/opt/toolchains/zephyr
      cd /workspace/hcp
      west build -p auto -d /workspace/hcp/build/${SLUG} -b ${BOARD} . -- \
        -DBOARD_ROOT=/workspace/hcp ${LED_ARGS} ${RS485_ARGS} ${VERSION_ARGS} ${BENCH_ARGS} ${EXTRA_ARGS}
    "
}

if [ "$BOARD_OR_ALL" = "all" ]; then
  for b in hcp_c5/esp32c5/hpcore hcp_c6/esp32c6/hpcore hcp_c3; do
    build_one "$b"
  done
else
  build_one "$BOARD_OR_ALL"
fi
