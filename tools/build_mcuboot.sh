#!/usr/bin/env bash
# Build MCUboot for an HCP board inside the Zephyr container.
#
# MCUboot is compiled as an ordinary Zephyr application from the west workspace's
# bootloader/mcuboot module, with the board's own devicetree supplying the slots -
# so one config (tools/mcuboot/mcuboot.conf) covers every board. Output is copied
# to build/mcuboot/<board>/mcuboot.bin for tools/flash.sh.
#
# Usage:
#   ./tools/build_mcuboot.sh                      # hcp_c6/esp32c6/hpcore
#   ./tools/build_mcuboot.sh hcp_c5/esp32c5/hpcore
#   ./tools/build_mcuboot.sh --signed hcp_c6/esp32c6/hpcore
#           ECDSA-P256 image verification on, with the key pair named by
#           HCP_SIGN_KEY (default root-ec256) under hcp/keys/. An application
#           image has to be signed with the same pair to boot: ./build.sh --signed.
#           See tools/mcuboot/README.md before mixing signed and unsigned images.
#
# Environment:
#   IMAGE   docker image to build in; the default dev image carries cbor2, which
#           imgtool needs to write the image trailer.
#   HCP_SIGN_KEY  key pair to sign with, without the .pem suffix.
set -euo pipefail

IMAGE="${IMAGE:-env-zephyr-espressif}"
HCP_SIGN_KEY="${HCP_SIGN_KEY:-root-ec256}"
BOARD="hcp_c6/esp32c6/hpcore"
SIGNED=0

while [ $# -gt 0 ]; do
  case "$1" in
    --signed) SIGNED=1; shift ;;
    -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
    hcp_c3|hcp_c5*|hcp_c6*) BOARD="$1"; shift ;;
    *) echo "unknown argument: $1" >&2; exit 1 ;;
  esac
done

HCP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"   # .../hcp
SLUG="$(echo "${BOARD}" | tr '/-' '__')"
OUT_DIR="${HCP_DIR}/build/mcuboot/${SLUG}"

CONF="/workspace/hcp/tools/mcuboot/mcuboot.conf"
if [ "${SIGNED}" = 1 ]; then
  CONF="${CONF};/workspace/hcp/tools/mcuboot/signing.conf"
  # MCUboot extracts the public key from this file itself (imgtool getpub), so it
  # needs the pair, not a pub-only file. Written as a conf fragment rather than a
  # -D argument: a string symbol passed through docker/bash/cmake loses its quotes
  # and Kconfig then rejects it as a "malformed string literal".
  mkdir -p "${HCP_DIR}/build"
  printf 'CONFIG_BOOT_SIGNATURE_KEY_FILE="/workspace/hcp/keys/%s.pem"\n' \
    "${HCP_SIGN_KEY}" > "${HCP_DIR}/build/.sign-mcuboot-${HCP_SIGN_KEY}.conf"
  CONF="${CONF};/workspace/hcp/build/.sign-mcuboot-${HCP_SIGN_KEY}.conf"
fi

docker run --rm \
  -v "$(dirname "${HCP_DIR}")":/workspace \
  --entrypoint /bin/bash "${IMAGE}" -lc "
    set -e
    source /opt/venv/bin/activate
    source /opt/toolchains/zephyr/zephyr-env.sh
    export ZEPHYR_BASE=/opt/toolchains/zephyr
    MCUBOOT_APP=/opt/toolchains/bootloader/mcuboot/boot/zephyr
    test -d \"\${MCUBOOT_APP}\" || { echo \"no MCUboot module at \${MCUBOOT_APP}\" >&2; exit 1; }
    cd /workspace/hcp
    west build -p always -d /workspace/hcp/build/mcuboot_${SLUG} -b ${BOARD} \${MCUBOOT_APP} -- \
      -DBOARD_ROOT=/workspace/hcp \
      -DEXTRA_CONF_FILE='${CONF}'
    mkdir -p /workspace/hcp/build/mcuboot/${SLUG}
    cp /workspace/hcp/build/mcuboot_${SLUG}/zephyr/zephyr.bin \
       /workspace/hcp/build/mcuboot/${SLUG}/mcuboot.bin
  "

ls -l "${OUT_DIR}/mcuboot.bin"
echo "next: ./tools/flash.sh ${BOARD%%/*}"
