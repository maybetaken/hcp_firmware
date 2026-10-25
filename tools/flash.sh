#!/usr/bin/env bash
# Copyright (c) 2026 maybetaken
# SPDX-License-Identifier: Apache-2.0

# Flash MCUboot plus the signed application to an HCP board, from the host
# (Docker cannot own the serial port on macOS).
#
#   ./tools/flash.sh                       # hcp_c6, port auto-detected
#   ./tools/flash.sh hcp_c5 -p /dev/cu.usbmodem101
#   ./tools/flash.sh hcp_c6 --no-erase     # skip the slot erase (never for a reflash)
#   ./tools/flash.sh hcp_c6 --app build/hcp_c6_esp32c6_hpcore_signed/zephyr/zephyr.signed.bin
#   ./tools/flash.sh hcp_c6 --mcuboot build/mcuboot/hcp_c6_esp32c6_hpcore/mcuboot.bin
#                                          # point at another artifact: build.sh puts
#                                          # --signed / --version / customized builds in
#                                          # their own directory, and signed and
#                                          # unsigned images must not be mixed with the
#                                          # wrong bootloader
#
# The primary slot is erased before writing, on purpose: MCUboot keeps its swap
# state in the trailer at the END of the slot and esptool only erases what it
# writes. Leaving a stale "test image, revert it" trailer behind makes the
# bootloader refuse to start a freshly flashed application - the board then looks
# dead on the console while the ROM bootloader is still perfectly reachable.
# See tools/mcuboot/README.md.
set -euo pipefail

HCP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BOARD="hcp_c6"
PORT=""
ERASE=1
BAUD="${BAUD:-460800}"
ESPTOOL="${ESPTOOL:-/Users/yabin/Documents/zephyr/venv/bin/esptool}"

while [ $# -gt 0 ]; do
  case "$1" in
    hcp_c3|hcp_c5|hcp_c6) BOARD="$1"; shift ;;
    -p|--port)  PORT="$2"; shift 2 ;;
    -b|--baud)  BAUD="$2"; shift 2 ;;
    --app)      APP="$2"; shift 2 ;;
    --mcuboot)  MCUBOOT="$2"; shift 2 ;;
    --no-erase) ERASE=0; shift ;;
    -h|--help)  sed -n 2,26p "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 1 ;;
  esac
done

case "${BOARD}" in
  hcp_c3) CHIP=esp32c3; BOOT_ADDR=0x000000; SLOT_ADDR=0x20000; SLOT_SIZE=$((0x1c0000)); SLUG=hcp_c3 ;;
  hcp_c6) CHIP=esp32c6; BOOT_ADDR=0x000000; SLOT_ADDR=0x20000; SLOT_SIZE=$((0x1c0000)); SLUG=hcp_c6_esp32c6_hpcore ;;
  # The C5's boot ROM loads the second stage from 0x2000, and its 8 MB partition
  # table gives the slots 3840 K each.
  hcp_c5) CHIP=esp32c5; BOOT_ADDR=0x002000; SLOT_ADDR=0x20000; SLOT_SIZE=$((0x3c0000)); SLUG=hcp_c5_esp32c5_hpcore ;;
esac

# Build directories are the board name with '/' and '-' turned into '_', which is
# what build.sh and build_mcuboot.sh produce.
MCUBOOT="${MCUBOOT:-${HCP_DIR}/build/mcuboot/${SLUG}/mcuboot.bin}"
APP="${APP:-${HCP_DIR}/build/${SLUG}/zephyr/zephyr.signed.bin}"

[ -n "${PORT}" ] || PORT="$(ls /dev/cu.usbmodem* 2>/dev/null | head -1 || true)"
[ -n "${PORT}" ] || { echo "no /dev/cu.usbmodem* found; is the board plugged in?" >&2; exit 1; }
[ -f "${MCUBOOT}" ] || { echo "missing ${MCUBOOT} - run ./tools/build_mcuboot.sh ${BOARD}/..." >&2; exit 1; }
if [ ! -f "${APP}" ]; then
  echo "missing ${APP}" >&2
  echo "  ${BOARD}: no signed image means the application was not linked for" >&2
  echo "  MCUboot (CONFIG_HCP_OTA, and with it BOOTLOADER_MCUBOOT, is off - see" >&2
  echo "  lib/ota/Kconfig), so there is no primary-slot image to write. Flash the" >&2
  echo "  plain binary to the boot address instead, no bootloader and no erase:" >&2
  echo "    esptool --chip ${CHIP} -p ${PORT} write_flash ${BOOT_ADDR} ${HCP_DIR}/build/${SLUG}/zephyr/zephyr.bin" >&2
  exit 1
fi

if [ "${BOARD}" = hcp_c5 ] || [ "${BOARD}" = hcp_c3 ]; then
  echo "note: only hcp_c6 has been run on hardware with this layout; expect to look"
  echo "      at the boot log the first time (${CHIP}, ${BOOT_ADDR} + ${SLOT_ADDR})."
fi

echo "board ${BOARD} (${CHIP}) port ${PORT}"
echo "  mcuboot ${MCUBOOT} -> ${BOOT_ADDR}"
echo "  app     ${APP} -> ${SLOT_ADDR}"

if [ "${ERASE}" = 1 ]; then
  "${ESPTOOL}" --chip "${CHIP}" -p "${PORT}" --before default_reset --after no_reset \
      erase_region "${SLOT_ADDR}" "${SLOT_SIZE}"
fi

"${ESPTOOL}" --chip "${CHIP}" -p "${PORT}" -b "${BAUD}" \
  --before default_reset --after hard_reset write_flash \
  "${BOOT_ADDR}" "${MCUBOOT}" \
  "${SLOT_ADDR}" "${APP}"

echo "done; watch the boot with ./monitor.sh"
