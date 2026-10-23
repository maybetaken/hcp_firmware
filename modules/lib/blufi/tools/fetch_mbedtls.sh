#!/usr/bin/env bash
# Fetch the Mbed TLS sources this module builds the optional encrypted
# provisioning path from, and pin them to the version it is tested against.
#
#   ./tools/fetch_mbedtls.sh                 # -> <module>/../.mbedtls-2.28
#   ./tools/fetch_mbedtls.sh /some/path      # anywhere you like
#
# The module ships no copy of Mbed TLS: nothing here is vendored, and the tree
# fetched by this script is the unmodified upstream. CMake finds it automatically
# at the default location (see CMakeLists.txt), or point it anywhere with
# -DBLUFI_MBEDTLS_DIR=<path>.
#
# Mbed TLS itself is Apache-2.0 (dual licensed GPL-2.0-or-later); this module's
# MIT license covers only this repository's files.
set -euo pipefail

TAG="v2.28.9"
COMMIT="5e146adef63b326b04282252639bebc2730939c6"   # what $TAG points at today
URL="https://github.com/Mbed-TLS/mbedtls.git"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"      # .../blufi
DEST="${1:-${HERE}/../.mbedtls-2.28}"

if [ -d "${DEST}/.git" ]; then
  echo "already fetched: ${DEST}"
  cd "${DEST}" && git log --oneline -1 && exit 0
fi

echo "cloning ${URL} ${TAG} -> ${DEST}"
git clone --depth 1 --branch "${TAG}" "${URL}" "${DEST}"

# A tag can be moved; the commit cannot. Fail loudly rather than build something
# other than what this module was tested with.
ACTUAL="$(git -C "${DEST}" rev-parse HEAD)"
if [ "${ACTUAL}" != "${COMMIT}" ]; then
  echo "unexpected revision at ${TAG}: ${ACTUAL}" >&2
  echo "expected ${COMMIT}; check the tag before pointing CMake at this tree"   >&2
  exit 1
fi

for f in dhm bignum constant_time aes md5; do
  test -f "${DEST}/library/${f}.c" || { echo "missing ${DEST}/library/${f}.c" >&2; exit 1; }
done

echo "ok: ${DEST} @ ${TAG} (${ACTUAL:0:7})"
echo "build with:  west build ... -- -DCONFIG_BLUFI_SECURITY=y"
