#!/usr/bin/env bash
# Create an MCUboot signing key pair in hcp/keys/ (inside the container, so no
# Python packages are needed on the host):
#
#   ./tools/make_signing_key.sh                 # keys/root-ec256.pem
#   ./tools/make_signing_key.sh bench-other     # keys/bench-other.pem
#
# The private key is what signs ./build.sh --signed images, and the public half is
# baked into MCUboot by ./tools/build_mcuboot.sh --signed. Everything under keys/ is
# gitignored on purpose: whoever holds this file can produce firmware that every
# device you have already flashed will accept, so treat it like a CA key - keep it
# out of the repository, out of backups you do not control, and generate a fresh one
# per product line rather than reusing this file. The full procedure, including
# what losing or leaking the key costs, is tools/mcuboot/SIGNING.md.
#
# MCUboot reads this private file and derives the public key itself (imgtool getpub)
# at build time; the .pub.c next to it is only a convenience copy of that output.
set -euo pipefail

HCP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KEY="${1:-root-ec256}"
IMAGE="${IMAGE:-env-zephyr-espressif}"

if [ -e "${HCP_DIR}/keys/${KEY}.pem" ]; then
  echo "keys/${KEY}.pem already exists - refusing to overwrite it" >&2
  echo "(losing a signing key means devices in the field can never be updated"   >&2
  echo " again without reflashing MCUboot; back the file up before deleting it)" >&2
  exit 1
fi

mkdir -p "${HCP_DIR}/keys"

docker run --rm -v "$(dirname "${HCP_DIR}")":/workspace --entrypoint /bin/bash \
  "${IMAGE}" -lc "
    set -e
    source /opt/venv/bin/activate
    cd /workspace/hcp/keys
    python3 /opt/toolchains/bootloader/mcuboot/scripts/imgtool.py keygen \
      -t ecdsa-p256 -k ${KEY}.pem
    python3 /opt/toolchains/bootloader/mcuboot/scripts/imgtool.py getpub \
      -k ${KEY}.pem > ${KEY}.pub.c
    chmod 600 ${KEY}.pem
  "

ls -l "${HCP_DIR}/keys/${KEY}.pem"
echo "now:  ./tools/build_mcuboot.sh --signed <board>   and   ./build.sh <board> --signed"
echo "     (both take HCP_SIGN_KEY=${KEY} if you are not using root-ec256)"
