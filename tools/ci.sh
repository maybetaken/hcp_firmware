#!/usr/bin/env bash
# Copyright (c) 2026 maybetaken
# SPDX-License-Identifier: Apache-2.0
# Continuous integration for HCP: four gates, one command, no service required.
#
#   ./tools/ci.sh                    every stage that can run here
#   ./tools/ci.sh licence release    only these stages
#
# Stages, and what each one exists to catch:
#
#   licence   tools/check_licence.py  - Apache-2.0 header on every source file, no empty
#             tracked file, no invisible U+3000 in a Kconfig help block
#   release   tools/check_release.py  - CONFIG_APP_VERSION, the imgtool spelling, the
#             Kconfig default, the CHANGELOG section and the git tag all agree
#   host      tests/host (ctest)      - the posix backends and the pure logic, 18 suites
#   firmware  ./build.sh all          - hcp_c3, hcp_c5 and hcp_c6, and warning-free
#
# host runs inside $HCP_CI_IMAGE when docker has it (that is the toolchain tests/host is
# written for) and natively otherwise. firmware always runs here, because build.sh is
# itself the docker wrapper. A stage that cannot run is a SKIP outside CI and a failure
# inside it - never a pass.
#
# Exit status is non-zero if any stage failed; the run always finishes and prints a
# summary, because "the first gate failed" is a useless answer when the third one was the
# interesting one.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$HERE")"                    # .../hcp
IMAGE="${HCP_CI_IMAGE:-env-zephyr-espressif}"
cd "$ROOT"

WANT=("$@")
[ ${#WANT[@]} -gt 0 ] || WANT=(licence release host firmware)

declare -a RESULTS=()
FAILED=0

have() { command -v "$1" >/dev/null 2>&1; }

run_stage() {
  local name="$1" fn="$2" rc=0
  printf '\n=== %s ===\n' "$name"
  if ! has_want "$name"; then
    RESULTS+=("skip  $name (not selected)")
    return 0
  fi
  "$fn" || rc=$?
  case "$rc" in
    0) RESULTS+=("pass  $name") ;;
    2) RESULTS+=("SKIP  $name (see the reason above)") ;;
    *) RESULTS+=("FAIL  $name"); FAILED=1 ;;
  esac
}

has_want() {
  local want
  for want in "${WANT[@]}"; do [ "$want" = "$1" ] && return 0; done
  return 1
}

# A stage that needs a compiler. The container comes first when it is available: these
# tests are written for the env image's toolchain (tests/host/CMakeLists.txt says so), and
# a host cmake on macOS gets as far as building GoogleTest and fails there, which reads as
# if the firmware's own tests were broken. Native is the fallback for a machine without
# docker - a plain Linux box with cmake and g++ runs them fine.
# Returns 2 when neither is available: a skip, unless CI asked for the run.
run_toolchain() {
  local body="$1"
  if have docker && docker image inspect "$IMAGE" >/dev/null 2>&1; then
    docker run --rm -v "$ROOT/..":/workspace --entrypoint /bin/bash "$IMAGE" \
      -lc "$(printf 'T=/workspace/hcp
%s' "$body")"
  elif have cmake && { have ninja || have make; } && have g++; then
    T="$ROOT" bash -c "$body"
  else
    echo "no docker image '$IMAGE' to run in and no usable cmake/g++ here: build the env"
    echo "image (Dockerfile.espressif in the zephyr environment repo) or install a toolchain."
    [ -n "${CI:-}" ] && return 1
    return 2
  fi
}

stage_licence() { have python3 || { echo "python3 needed"; return 1; }; python3 tools/check_licence.py; }

stage_release() { have python3 || { echo "python3 needed"; return 1; }; python3 tools/check_release.py; }

stage_host() {
  # Built in the container's own /tmp rather than under the mounted tree: the bind mount is
  # slow for the many small files of a build, and FetchContent's download of GoogleTest
  # fails there in a way it does not fail on the container filesystem.
  run_toolchain '
    set -e
    rm -rf /tmp/hcp-ci-hosttests
    cmake -S "$T/tests/host" -B /tmp/hcp-ci-hosttests > /tmp/hcp-ci-cfg.log 2>&1 || { tail -30 /tmp/hcp-ci-cfg.log; exit 1; }
    cmake --build /tmp/hcp-ci-hosttests -j 4 > /tmp/hcp-ci-build.log 2>&1 || { tail -30 /tmp/hcp-ci-build.log; exit 1; }
    ctest --test-dir /tmp/hcp-ci-hosttests --output-on-failure | tail -5
  '
}

# Not run_toolchain: build.sh is itself a docker wrapper, so calling it inside a container
# asks for a docker daemon that is not there. It runs here, on whatever machine is running
# the CI, and needs docker (or a local Zephyr toolchain, which build.sh would then want
# too).
stage_firmware() {
  if ! have docker; then
    echo "firmware builds go through ./build.sh, which needs docker on this machine."
    [ -n "${CI:-}" ] && return 1
    return 2
  fi
  ./build.sh all > build/ci-boards.log 2>&1 || { tail -40 build/ci-boards.log; return 1; }
  # The same gate tools/release.sh applies: any compiler diagnostic that is not the known
  # MCUboot version-string warning fails the build. A warning today is the error that eats
  # someone else's afternoon.
  if grep -E 'error:|warning:' build/ci-boards.log | grep -qv MCUBOOT_IMGTOOL; then
    grep -E 'error:|warning:' build/ci-boards.log | grep -v MCUBOOT_IMGTOOL | head -8
    return 1
  fi
  grep -E 'FLASH:|sram0_0_seg' build/ci-boards.log | tail -6
  echo "three boards built warning-free"
}

run_stage licence  stage_licence
run_stage release  stage_release
run_stage host     stage_host
run_stage firmware stage_firmware

echo
echo "================ ci summary ================"
for r in "${RESULTS[@]}"; do echo "  $r"; done
[ "$FAILED" = 0 ] && echo "  => green" || echo "  => FAILED"
exit $FAILED
