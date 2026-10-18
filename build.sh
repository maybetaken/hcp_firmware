#!/usr/bin/env bash
# Build the HCP app for a target board inside the env-zephyr-espressif image
# (one-shot container; output persists to host under hcp/build/<slug>).
#
# Usage:
#   ./build.sh                                  # default board
#   ./build.sh hcp_c6/esp32c6/hpcore
#   ./build.sh hcp_c3
#   ./build.sh hcp_c5/esp32c5/hpcore
set -euo pipefail

BOARD="${1:-hcp_c6/esp32c6/hpcore}"
IMAGE="env-zephyr-espressif"
EXTRA="${2:-}"   # optional extra west args, e.g. -- -DEXTRA_CONF_FILE=...

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
    west build -p auto -d /workspace/hcp/build/${SLUG} -b ${BOARD} . -- -DBOARD_ROOT=/workspace/hcp
  "
