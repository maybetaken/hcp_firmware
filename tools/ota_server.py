#!/usr/bin/env python3
# Copyright (c) 2026 maybetaken
# SPDX-License-Identifier: Apache-2.0

"""Serve HCP OTA images to the bench - threaded, because two boards are not one device.

    ./tools/ota_server.py                    # build/otasrv on :9998
    ./tools/ota_server.py --port 9999

Why not `python3 -m http.server`: it answers one connection at a time, so with two boards
on the bench the second one's GET queued behind ~900 kB of transfer and timed out - seen
as "ota":"error (-116)" on a device whose image was fine. Per-board staging directories
decide *which* file a device gets; this decides whether it can get it at all.
"""

import argparse
import functools
import os
import socketserver
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(os.path.dirname(HERE), "build", "otasrv")


class Handler(SimpleHTTPRequestHandler):
    """One line per request, which tells a stalled release apart from a slow one."""

    def log_message(self, fmt, *args):
        print("[ota-server] %s %s" % (self.address_string(), fmt % args), flush=True)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", default=ROOT, help="directory to serve (default build/otasrv)")
    ap.add_argument("--port", type=int, default=9998)
    ap.add_argument("--bind", default="0.0.0.0")
    args = ap.parse_args()

    if not os.path.isdir(args.root):
        raise SystemExit(f"no such directory to serve: {args.root}")

    handler = functools.partial(Handler, directory=args.root)
    socketserver.TCPServer.allow_reuse_address = True
    with ThreadingHTTPServer((args.bind, args.port), handler) as srv:
        print(f"[ota-server] serving {args.root} on http://{args.bind}:{args.port}/ "
              "(threaded)", flush=True)
        srv.serve_forever()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
