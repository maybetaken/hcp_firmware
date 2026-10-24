# Firmware versions

Every released firmware version has an entry here **and** a git tag `v<version>`.
`./tools/release.sh <version>` enforces that pairing: it refuses to bump the version
or create a tag when this file has no `## <version>` section, because a number that
nothing describes is not a release - it is just a rebuild.

## The version line

`major.minor.patch.build`, four dotted numbers, starting at 2.0.0.0. Two consumers
read it and they do not accept the same spelling:

| where | symbol | spelling | checked by |
|---|---|---|---|
| what the device reports (`<sn>/online`, `<sn>-K/online`, diagnostics) and what `hcp::ota` compares against the server's `version.txt` | `CONFIG_APP_VERSION` | `2.0.3.14` | `ota_client`'s `VERSION_FIELDS = 4` |
| the MCUboot image header (its downgrade prevention) | `CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION` | `2.0.3+14` | `imgtool`'s regex: `maj.min.rev[+build]` |

That second spelling is not a style choice. `scripts/imgtool/version.py` in the
MCUboot tree rejects a dotted fourth field ("Invalid version number, should be
maj.min.rev+build") while the header itself carries `major(B) minor(B) revision(H)
build(I)` - so `x.y.z.w` has to arrive as `x.y.z+w`. `build.sh --version` does that
translation, and `tools/release.sh` writes both symbols, always together.

An image is only accepted by a device over the network when its version is
**strictly greater** than what that device runs. Re-flashing the same number is a
USB operation, not an OTA one - which is worth knowing before cutting a build to
test on the bench.

## How to release

```sh
# 1. write the entry below first - what changed, and what was measured
# 2. bump, build, commit and tag in one step:
./tools/release.sh 2.0.3.15
# 3. put it on a device:
HCP_SN=hcp-M1231 ./tools/ota_release.sh 2.0.3.15 hcp_c3 --bench
```

`release.sh` builds every board before it commits or tags, so a tag cannot point at
a tree that does not compile. There is no git remote yet, so nothing is pushed: when
one exists, `git push && git push --tags` after the release.

## 2.0.3.14 — 2026-10-24

The Home Assistant contract is complete, and the relay outputs became the second
device they were always meant to be.

- **`<sn>-K`, the relay device.** Two topic trees in `hcp::mqtt` (`<sn>/#` and
  `<sn>-K/#`), routed on `(tree, suffix)` because `control/cmd` means a raw Modbus
  write under `<sn>` and a JSON switch command under `<sn>-K`. Announces
  `{"switch_count": 2, "version": ...}` on every connect, drives `hcp-k1`/`hcp-k2`,
  persists them (`relay/k1`, `relay/k2`) and restores them before the first report,
  and answers with a full `<sn>-K/notify` snapshot - re-sent whenever the session
  comes up, because QoS 0 leaves nothing queued. Deliberately absent:
  `<sn>-K/diagnostics` (attached devices have diagnostics off) and the old
  `<sn>-K/status/K1|K2`, which this integration never subscribes.
- **Diagnostics carries `ssid` and `rssi`.** `hcp::net::wifi::GetLinkInfo()` via
  `NET_REQUEST_WIFI_IFACE_STATUS`; the ESP32 port fills both from
  `esp_wifi_sta_get_ap_info()`, so the signal reading is live rather than the value
  remembered from the scan. Home Assistant reads exactly `ssid`, `rssi` and `led`
  from this document, and an SSID containing `"` or a control byte would have made
  the whole document unparseable, so it is sanitised on the way in.
- **Version line moved to 2.0.x.y**, with the imgtool spelling split documented above.
- **LED and button: measured, and both reports were something else.** The 50 ms tick
  runs at exactly 50 ms, GPIO10 idles at the level the devicetree says, and the C3's
  bit-bang strip alternates `rgb(6,15,25)`/`rgb(0,0,0)` every ~255 ms in blue-fast -
  so a steady light is the connected pattern (`kGreenSolid`), not a dead driver, and
  no RMT port was needed. Left in as evidence: the tick line, the button's idle and
  settled-edge lines, and `led flush rgb(...) at N%`, which prints the bytes that
  actually went to the strip (it also confirmed the 10 % brightness scaling:
  80,255,120 → 8,25,12).
- Verified on the bench C3: cross-tree isolation (main tree + JSON → `write request
  rejected`; `<sn>-K` + binary → `names no switch`), `control/led on|off` reflected in
  diagnostics, host tests 18/18, zero warnings for hcp_c3/hcp_c5/hcp_c6 and the
  c6 `--rs485` variant.

## Before 2.0.0.0 (the 0.x bring-up line, untagged)

Numbered `0.1.0` … `0.3.13` while the port was still being assembled; these were
bench images and none was tagged, so they are recorded here as one block rather than
invented history. What they contain, newest first:

- `0.3.x` — the business layer: `hcp::registry` (point table, download rules, the
  big-endian `notify`/`control/cmd` byte layout with real payload vectors),
  `hcp::collector` (one queue, one worker owning the bus, per-segment schedule, dead
  slave backoff, heartbeat armed by the first beat), `<sn>/diagnostics` replacing the
  invented `<sn>/state`, `<sn>/ota/start` routed, the native RTU frame engine
  (`lib/modbus` `BACKEND_NATIVE`) after Zephyr's subsys rejected a provably correct
  29-byte reply from the bench meter, the C3 WS2812 moved to GPIO bit-bang, and the
  fix for a topic pointer that the payload read was overwriting (which had made every
  topic longer than ~6 bytes unreachable from Home Assistant).
- `0.2.0` — network releases: `tools/ota_release.sh` and a bench image that stays on
  the network, so the first flash is the only one that needs the USB port.
- `0.1.0` — the modularised `src/` (one `App` instance, a class per concern), the
  bench image (`--bench`), signed OTA with trial rollback, MCUBOOT signing docs.
- Earlier still: the pre-split `main.cpp`, P0 bring-up, Wi-Fi/BluFi/SSDP/MQTT/OTA
  modules, and the three custom boards (hcp_c6 / hcp_c3 / hcp_c5).
