#!/usr/bin/env bash
# Copyright (c) 2026 maybetaken
# SPDX-License-Identifier: Apache-2.0

# Release a firmware image to a device over the network - build, stage, trigger.
#
#   ./tools/ota_release.sh 0.3.0                       # hcp_c6, product build
#   ./tools/ota_release.sh 0.3.0 --bench               # bench build (aids, dbg log)
#   ./tools/ota_release.sh 0.3.0 hcp_c3 --no-trigger    # stage only
#   HCP_SN=hcp-M1230 ./tools/ota_release.sh 0.3.0
#
# What it does:
#   1. builds the board with --signed --version <v> (both version stamps move together;
#      an unsigned image will not boot under a signed MCUboot);
#   2. stages zephyr.signed.bin as build/otasrv/<soc>/v<version>/firmware.bin with
#      version.txt beside it - the two names hcp::ota expects under a base URL;
#   3. publishes "ota <that URL>" on <sn>/cmd, so the device pulls from here rather than
#      from the host it learned over SSDP (right in production, wrong on a bench where
#      the Mac is the file server).
#
# One directory per board and per version because one flat directory is one image: two
# boards on the bench are not one device, and a shared firmware.bin means the second
# release overwrites what the first is still downloading.
#
# Override the address the device reaches us on with OTA_HOST=...
#
# The MCUboot swap takes ~70 s on this flash and prints nothing while it runs, so the
# script waits for <sn>/online to come back carrying the version it was given: a trigger
# is either confirmed or reported as unconfirmed, never assumed. The device only confirms
# an image once it has a live broker session, and the boot watchdog reverts it otherwise -
# a bad release comes back by itself, and the USB port stays in the drawer.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"   # .../hcp
SRV_ROOT="${HERE}/build/otasrv"   # what ./tools/ota_server.py serves
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

# A bare board name is what people type, but two of the three boards are multi-target and
# Zephyr refuses them without the SoC/CPU qualifier ("hcp_c6" died in cmake). The C3 needs
# no qualifier, which is why this was only found when the second board came onto the bench.
case "$BOARD" in
  hcp_c5) BOARD="hcp_c5/esp32c5/hpcore" ;;
  hcp_c6) BOARD="hcp_c6/esp32c6/hpcore" ;;
esac

# Which tree of the file server this board reads from; the SoC field of an explicit target
# is taken as given.
case "$BOARD" in
  hcp_c3*) SOC=esp32c3 ;;
  hcp_c5*) SOC=esp32c5 ;;
  hcp_c6*) SOC=esp32c6 ;;
  *) SOC="${BOARD#*/}"; SOC="${SOC%%/*}" ;;
esac
SRV_DIR="${SRV_ROOT}/${SOC}/v${VERSION}"
OTA_URL="http://${SRV_HOST}:${SRV_PORT}/${SOC}/v${VERSION}/"

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
echo ">>> staged ${VERSION} ($(wc -c < "$SRV_DIR/firmware.bin" | tr -d ' ') bytes) at ${OTA_URL}"

if ! lsof -nP -iTCP:${SRV_PORT} -sTCP:LISTEN >/dev/null 2>&1; then
  cat >&2 <<EOF
nothing is listening on :${SRV_PORT} - the device cannot fetch the image.
Start the file server in another terminal (threaded, so two boards do not queue behind
each other - a queued GET is what an "ota":"error (-116)" on the device actually is):
  cd ${HERE} && ./tools/ota_server.py
EOF
  exit 1
fi

if [ "$TRIGGER" = "0" ]; then
  echo ">>> not triggered; when ready:"
  echo "    ${HERE}/tools/mqtt_watch.py pub '${SN}/cmd' 'ota ${OTA_URL}'"
  exit 0
fi

echo ">>> triggering ${SN} from ${OTA_URL}"
"${HERE}/tools/mqtt_watch.py" pub "${SN}/cmd" "ota ${OTA_URL}"

# Confirm it, or say so: wait for <sn>/online to come back carrying the version that was
# asked for, so a trigger is confirmed or reported as unconfirmed, never assumed. Ending
# with "watch it yourself" is how a release once looked failed because the log window had
# closed before the swap finished. Budget = download + ~70 s swap + confirm window.
echo ">>> waiting for ${SN}/online to report ${VERSION} (download + swap, up to 6 min)"
DEADLINE=$(( $(date +%s) + 360 ))
while [ "$(date +%s)" -lt "$DEADLINE" ]; do
  # The payload has to match, not just any occurrence: <sn>/online is retained, so a
  # subscriber gets the last version whether or not the device ever came back.
  if "${HERE}/tools/mqtt_watch.py" sub "${SN}/online" 20 2>/dev/null |
     grep -qE " -> +${VERSION}[[:space:]]*$"; then
    echo ">>> ${SN} is running ${VERSION}"
    echo "    (it confirms the image ~60 s after its broker session comes up; do not"
    echo "     reset it before that, or MCUboot rolls the trial back by design)"
    exit 0
  fi
  sleep 5
done

cat >&2 <<EOF
>>> ${SN} never came back reporting ${VERSION}.
Either the download failed or it is still swapping. Check without touching the box:
  ${HERE}/tools/mqtt_watch.py sub '${SN}/#' 60          # "ota":"error (-nnn)" says how it failed
  ${HERE}/tools/mqtt_watch.py sub '${SN}/online' 60     # a rollback shows the old version return
Do not reset it while a trial swap is pending - that is what rollback is for.
EOF
exit 1
