#!/usr/bin/env python3
"""Serve HCP OTA images to the bench - threaded, because two boards are not one device.

    ./tools/ota_server.py                    # build/otasrv on :9998
    ./tools/ota_server.py --port 9999

The reason this exists instead of `python3 -m http.server 9998 -d build/otasrv`: that
server accepts one connection at a time. With two boards on the bench, the second one's
GET sat queued behind the first one's ~900 kB download and gave up - measured on an
ESP32-C3 as `"ota":"error (-116)"` (Zephyr's ETIMEDOUT) on hcp-M1231 while hcp-M1230 was
still pulling its own image. The per-board staging directories
(esp32c3/v2.0.3.20/, esp32c6/v2.0.3.20/) fixed *which* file a device gets; this fixes the
server being able to answer two devices at once.
"""

import argparse
import functools
import os
import socketserver
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(os.path.dirname(HERE), "build", "otasrv")


class Handler(SimpleHTTPRequestHandler):
    """One line per request, which is what tells a stalled release apart from a slow one."""

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
