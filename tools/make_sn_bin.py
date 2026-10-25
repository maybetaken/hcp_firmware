#!/usr/bin/env python3
# Copyright (c) 2026 maybetaken
# SPDX-License-Identifier: Apache-2.0

"""Make (and optionally burn) the per-device factory SN blob.

hcp::identity reads the "prodinfo" partition first, so a device can carry its own
serial number without rebuilding the application image: one common zephyr.bin for
every unit, plus a 4 KB sn.bin burned per unit.

Layout (little-endian, 0xFF where unwritten):

    0   magic  'H','C','P','1'
    4   u16    length of the factory code
    6   code   raw ASCII, no NUL on flash

The firmware prefixes the code with CONFIG_HCP_IDENTITY_PREFIX ("hcp-" unless the
build changed it) unless the code already starts with it, so "M1230" and
"hcp-M1230" both end up as hcp-M1230 - on the broker topics and, lowercased, in
the DHCP hostname the router shows.

    ./tools/make_sn_bin.py M1230                          # write sn.bin only
    ./tools/make_sn_bin.py M1230 --board hcp_c6 --burn     # and flash the partition
    ./tools/make_sn_bin.py M1230 --out /tmp/sn.bin --size 4096

Burning prodinfo does not touch the "storage" partition's data: prodinfo was carved
out of its tail and storage kept its start offset, so Wi-Fi credentials survive.
The device picks the new number up on the next boot.

esptool comes from the host venv (HCP_ESPTOOL overrides its path); the port has to
be free (close monitor.sh / miniterm first).
"""

import argparse
import os
import struct
import subprocess
import sys

# prodinfoPartition offsets, from boards/helios/*/*.dts (tail of "storage").
BOARDS = {
    "hcp_c6": {"offset": 0x3DF000, "chip": "esp32c6"},
    "hcp_c3": {"offset": 0x3DF000, "chip": "esp32c3"},
    "hcp_c5": {"offset": 0x7DF000, "chip": "esp32c5"},
}

MAGIC = b"HCP1"
DEFAULT_PREFIX = "hcp-"
VENV_ESPTOOL = "/Users/yabin/Documents/zephyr/venv/bin/esptool"


def build_blob(code: str, prefix: str, size: int) -> bytes:
    payload = code.encode("ascii")
    if prefix and not code.startswith(prefix):
        full = prefix + code
    else:
        full = code
    if len(full) > 32 - 1:  # keep within identity::kSerialMax
        raise SystemExit(
            f"serial '{full}' is {len(full)} chars; the firmware accepts 32")
    if len(payload) > 0xFFFF:
        raise SystemExit("factory code too long")
    blob = bytearray(b"\xFF" * size)
    blob[0:4] = MAGIC
    blob[4:6] = struct.pack("<H", len(payload))
    blob[6 : 6 + len(payload)] = payload
    print(f"[sn] burning code '{code}' -> device will call itself '{full}'")
    return bytes(blob)


def flash(path: str, board: str, port: str, baud: int) -> int:
    spec = BOARDS[board]
    esptool = os.environ.get("HCP_ESPTOOL", VENV_ESPTOOL)
    cmd = [
        esptool, "--chip", spec["chip"], "-p", port, "-b", str(baud),
        "--before", "default_reset", "--after", "hard_reset",
        "write_flash", hex(spec["offset"]), path,
    ]
    print("[sn] " + " ".join(cmd))
    return subprocess.call(cmd)


def main(argv) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("code", help="factory serial code, e.g. M1230")
    ap.add_argument("--out", default="sn.bin", help="blob to write (default sn.bin)")
    ap.add_argument("--prefix", default=DEFAULT_PREFIX,
                    help=f"firmware prefix (default {DEFAULT_PREFIX!r}); must match "
                         "CONFIG_HCP_IDENTITY_PREFIX")
    ap.add_argument("--size", type=int, default=4096,
                    help="blob size, the prodinfo partition (default 4096)")
    ap.add_argument("--board", choices=sorted(BOARDS),
                    help="with --burn: which board decides the flash offset")
    ap.add_argument("--burn", action="store_true", help="flash it with esptool")
    ap.add_argument("--port", default="/dev/cu.usbmodem101", help="serial port")
    ap.add_argument("--baud", type=int, default=460800, help="flash baud")
    args = ap.parse_args(argv[1:])

    if args.size < 6 + len(args.code):
        raise SystemExit("--size too small for the header plus the code")

    blob = build_blob(args.code, args.prefix, args.size)
    with open(args.out, "wb") as fh:
        fh.write(blob)
    print(f"[sn] wrote {args.out} ({args.size} bytes)")

    if not args.burn:
        if args.board:
            print(f"[sn] offset for {args.board}: {hex(BOARDS[args.board]['offset'])}")
        else:
            print("[sn] add --board <hcp_c3|hcp_c5|hcp_c6> --burn to flash it directly")
        return 0
    if not args.board:
        raise SystemExit("--burn needs --board to know the flash offset")
    return flash(args.out, args.board, args.port, args.baud)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
