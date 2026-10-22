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
#
# Environment:
#   IMAGE   docker image to build in; the default dev image carries cbor2, which
#           imgtool needs to write the image trailer.
set -euo pipefail

BOARD="${1:-hcp_c6/esp32c6/hpcore}"
IMAGE="${IMAGE:-env-zephyr-espressif}"
HCP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"   # .../hcp
SLUG="$(echo "${BOARD}" | tr '/-' '__')"
OUT_DIR="${HCP_DIR}/build/mcuboot/${SLUG}"

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
      -DEXTRA_CONF_FILE=/workspace/hcp/tools/mcuboot/mcuboot.conf
    mkdir -p /workspace/hcp/build/mcuboot/${SLUG}
    cp /workspace/hcp/build/mcuboot_${SLUG}/zephyr/zephyr.bin \
       /workspace/hcp/build/mcuboot/${SLUG}/mcuboot.bin
  "

ls -l "${OUT_DIR}/mcuboot.bin"
echo "next: ./tools/flash.sh ${BOARD%%/*}"
