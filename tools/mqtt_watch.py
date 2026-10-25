#!/usr/bin/env python3
# Copyright (c) 2026 maybetaken
# SPDX-License-Identifier: Apache-2.0

"""Watch or drive the HCP device over MQTT, with no third-party packages.

On the wire (subscribed set: lib/collector/src/commands.cpp, copied topic by topic from
Home Assistant's plugins):

  published   <sn>/online        retained, firmware version, every session
                <sn>/diagnostics periodic: ssid, rssi, led, uptime, ota
                <sn>/notify      the collected registers
                <sn>-K/online    {"switch_count":N,...}   <sn>-K/notify  relay snapshot
  subscribed  <sn>/cmd           "ota [http://host:port/]" - the bench update channel
                <sn>/config, <sn>/control/{cmd,restart,reconfig,led}, <sn>/host/heartbeat,
                <sn>/ota/start, <sn>-K/control/cmd

Nothing is echoed back: no <sn>/ack, and <sn>/state became <sn>/diagnostics - a topic
nobody subscribes to is a heartbeat published into the void.

Two traps that cost bench time: "-K" is a sibling tree, so '<sn>/#' does NOT match it; and
<sn>/notify is not retained, so subscribing after publishing shows a silent device.

This speaks MQTT 3.1.1 over a plain TCP socket - enough to see what a device sends and to
make it react, useful when the USB console is not attached.

    ./tools/mqtt_watch.py sub 'hcp-M1230/#' 60
    ./tools/mqtt_watch.py pub 'hcp-M1230/cmd' 'ota http://192.168.31.74:9998/'

Broker and credentials default to prj.conf / Home Assistant's mosquitto add-on; override
with MQTT_HOST / MQTT_PORT / MQTT_USER / MQTT_PASS.
"""

import os
import socket
import struct
import sys
import time
import uuid

HOST = os.environ.get("MQTT_HOST", "192.168.31.110")
PORT = int(os.environ.get("MQTT_PORT", "1883"))
USER = os.environ.get("MQTT_USER", "maybetaken").encode()
PASS = os.environ.get("MQTT_PASS", "maybetaken").encode()
# Unique per process. This used to be the current second, so two boards released at once
# collided - and MQTT closes the older session when a client id reconnects, which arrived
# as "broker closed the connection" and killed the verification of an update that had in
# fact succeeded on the device.
CLIENT_ID = b"hcp-watch-" + uuid.uuid4().hex[:8].encode()


def _binstr(data: bytes) -> bytes:
    return struct.pack("!H", len(data)) + data


def _remaining_length(n: int) -> bytes:
    out = b""
    while True:
        digit = n % 128
        n //= 128
        if n:
            digit |= 0x80
        out += bytes([digit])
        if not n:
            return out


def _recv_exact(sock: socket.socket, n: int) -> bytes:
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("broker closed the connection")
        buf += chunk
    return buf


def _read_packet(sock: socket.socket):
    header = _recv_exact(sock, 1)[0]
    multiplier = 1
    length = 0
    while True:
        byte = _recv_exact(sock, 1)[0]
        length += (byte & 0x7F) * multiplier
        multiplier *= 128
        if not (byte & 0x80):
            break
    return header, _recv_exact(sock, length)


def connect() -> socket.socket:
    sock = socket.create_connection((HOST, PORT), timeout=10)
    variable = b"\x00\x04MQTT" + bytes([0x04, 0xC2]) + struct.pack("!H", 60)
    payload = _binstr(CLIENT_ID) + _binstr(USER) + _binstr(PASS)
    body = variable + payload
    sock.sendall(b"\x10" + _remaining_length(len(body)) + body)
    header, ack = _read_packet(sock)
    if (header & 0xF0) >> 4 != 2 or len(ack) < 2 or ack[1] != 0:
        detail = ack[1] if len(ack) > 1 else "?"
        raise RuntimeError(f"CONNACK refused (return code {detail})")
    print(f"[watch] connected to {HOST}:{PORT} as {CLIENT_ID.decode()}", flush=True)
    return sock


def subscribe(sock: socket.socket, topic: str) -> None:
    body = struct.pack("!H", 1) + _binstr(topic.encode()) + b"\x00"
    sock.sendall(b"\x82" + _remaining_length(len(body)) + body)


def publish(sock: socket.socket, topic: str, message: str) -> None:
    body = _binstr(topic.encode()) + message.encode()
    sock.sendall(b"\x30" + _remaining_length(len(body)) + body)


def loop(sock: socket.socket, seconds: float) -> None:
    deadline = None if seconds <= 0 else time.time() + seconds
    sock.settimeout(1.0)
    last_ping = time.time()
    while True:
        if deadline is not None and time.time() > deadline:
            return
        if time.time() - last_ping > 30:
            sock.sendall(b"\xC0\x00")
            last_ping = time.time()
        try:
            header, body = _read_packet(sock)
        except socket.timeout:
            continue
        kind = (header & 0xF0) >> 4
        if kind == 3 and len(body) >= 2:
            topic_len = struct.unpack("!H", body[:2])[0]
            topic = body[2:2 + topic_len].decode(errors="replace")
            payload = body[2 + topic_len:]
            # <sn>/notify carries a raw Modbus ADU, not text. Decoding it as UTF-8
            # is not enough to tell: a frame like 01 04 00 1A ... is valid UTF-8 made
            # of control characters, and printed as text it looks like an empty
            # message - which reads as "the device sent nothing". Anything with a
            # non-printable byte is shown as hex with its length.
            printable = payload and all(32 <= b < 127 for b in payload)
            if printable:
                shown = payload.decode("ascii")
            else:
                shown = f"[{len(payload)} B] {payload.hex(' ').upper()}"
            print(f"[{time.strftime('%H:%M:%S')}] {topic}  ->  {shown}", flush=True)
        elif kind == 13:
            pass  # PINGRESP
        elif kind == 9:
            print("[watch] subscribed", flush=True)


def usage() -> int:
    print(__doc__)
    return 2


def main(argv) -> int:
    if len(argv) < 2:
        return usage()
    action = argv[1]
    if action == "sub" and len(argv) >= 3:
        topic, seconds = argv[2], float(argv[3]) if len(argv) > 3 else 0
    elif action == "pub" and len(argv) >= 4:
        topic, message, seconds = argv[2], argv[3], 5
    else:
        return usage()

    sock = connect()
    subscribe(sock, topic)
    if action == "pub":
        publish(sock, topic, message)
        print(f"[watch] published to {topic}: {message}", flush=True)
    try:
        loop(sock, seconds)
    except KeyboardInterrupt:
        pass
    except ConnectionError as exc:
        # The watch ending is not the command failing: the SUBSCRIBE and any PUBLISH have
        # already gone out, so a broker dropping this observer must not read as "the
        # device did not do it".
        print(f"[watch] observation ended early: {exc}", flush=True)
    finally:
        sock.close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
