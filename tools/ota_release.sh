#!/usr/bin/env bash
# Release a firmware image to a device over the network - build, stage, trigger.
#
#   ./tools/ota_release.sh 0.3.0                       # hcp_c6, product build
#   ./tools/ota_release.sh 0.3.0 --bench               # bench build (aids, dbg log)
#   ./tools/ota_release.sh 0.3.0 hcp_c3 --no-trigger    # stage only
#   HCP_SN=hcp-M1230 ./tools/ota_release.sh 0.3.0
#
# What it does:
#   1. builds the board with --signed --version <v> (both version stamps move
#      together; an unsigned image will not boot under a signed MCUboot);
#   2. stages zephyr.signed.bin as build/otasrv/firmware.bin with its version in
#      version.txt - the two file names hcp::ota expects under a base URL;
#   3. publishes "ota http://<this host>:9998/" on <sn>/cmd so the device pulls it
#      from here rather than from the host it learned over SSDP (a plain "ota"
#      means "use the discovered host", which is right in production and wrong on a
#      bench where the Mac is the file server).
#
# Override the server address with OTA_HOST=... if this machine's address is not
# what the device should reach.
#
# The device confirms the image only once it has a live broker session, and the
# watchdog hcp::ota arms at boot reverts it otherwise - so a bad release comes
# back by itself, and the USB port stays in the drawer.
#
# The MCUboot swap takes ~70 s on this flash and prints nothing while it runs;
# watch for the device reappearing on <sn>/online with the new version.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"   # .../hcp
SRV_DIR="${HERE}/build/otasrv"
SRV_PORT=9998
SN="${HCP_SN:-hcp-M1230}"
# The address the device should reach us on: the LAN address of this machine,
# unless the bench says otherwise (port-forwarded, running in a container, ...).
SRV_HOST="${OTA_HOST:-$(ipconfig getifaddr en0 2>/dev/null || echo '')}"
if [ -z "$SRV_HOST" ]; then
  echo "cannot work out this host's address; set OTA_HOST=<ip the device can reach>" >&2
  exit 1
fi

VERSION="${1:-}"
case "$VERSION" in
  ""|-h|--help)
    sed -n '2,26p' "$0"; exit 2 ;;
esac
shift
BOARD="hcp_c6/esp32c6/hpcore"
TRIGGER=1
BUILD_ARGS=""
while [ $# -gt 0 ]; do
  case "$1" in
    --no-trigger) TRIGGER=0; shift ;;
    --bench)      BUILD_ARGS="--bench"; shift ;;
    *)            BOARD="$1"; shift ;;
  esac
done

echo ">>> building ${BOARD} ${BUILD_ARGS} as signed ${VERSION}"
BUILD_LOG="$(mktemp)"
if ! "${HERE}/build.sh" "$BOARD" --signed --version "$VERSION" ${BUILD_ARGS} > "$BUILD_LOG" 2>&1; then
  tail -25 "$BUILD_LOG"
  echo "build failed" >&2
  exit 1
fi
# Take the directory from build.sh rather than reconstructing its naming rules.
BUILD_DIR="$(sed -n 's/.*-> build\/\([^ ]*\)$/\1/p' "$BUILD_LOG" | tail -1)"
IMAGE="${HERE}/build/${BUILD_DIR}/zephyr/zephyr.signed.bin"
[ -f "$IMAGE" ] || { echo "no signed image at ${IMAGE}" >&2; exit 1; }

mkdir -p "$SRV_DIR"
cp "$IMAGE" "$SRV_DIR/firmware.bin"
printf '%s\n' "$VERSION" > "$SRV_DIR/version.txt"
echo ">>> staged ${VERSION} ($(wc -c < "$SRV_DIR/firmware.bin" | tr -d ' ') bytes) in build/otasrv"

if ! lsof -nP -iTCP:${SRV_PORT} -sTCP:LISTEN >/dev/null 2>&1; then
  cat >&2 <<EOF
nothing is listening on :${SRV_PORT} - the device cannot fetch the image.
Start the file server in another terminal:
  cd ${HERE} && python3 -m http.server ${SRV_PORT} --bind 0.0.0.0 -d build/otasrv
EOF
  exit 1
fi

if [ "$TRIGGER" = "0" ]; then
  echo ">>> not triggered; when ready:"
  echo "    ${HERE}/tools/mqtt_watch.py pub '${SN}/cmd' 'ota http://${SRV_HOST}:${SRV_PORT}/'"
  exit 0
fi

echo ">>> triggering ${SN} from http://${SRV_HOST}:${SRV_PORT}/"
"${HERE}/tools/mqtt_watch.py" pub "${SN}/cmd" "ota http://${SRV_HOST}:${SRV_PORT}/"
echo ">>> watch it: ${HERE}/tools/mqtt_watch.py sub '${SN}/#'   (swap takes ~70 s, silent)"
