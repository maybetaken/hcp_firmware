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

## 2.0.3.15 — 2026-10-24

The C3's status LED could light up but not blink or switch - and now it can, because
the timings were measured instead of assumed.

- **Root cause.** The bit-bang loop bounded each phase by reading the system clock,
  and on this part that primitive is slower than the phase it is meant to bound:
  measured with interrupts locked, `sys_clock_cycle_get_32()` costs **687 ns** and
  `gpio_pin_set_dt()` **312 ns**, while a WS2812 bit period is 1250 ns and a zero's
  high phase is 400 ns. Every bit therefore came out at the same ~2000 ns floor (two
  reads plus two calls), which a strip reads as all-ones whatever was asked for - lit,
  stuck, immune to data. No choice of constants could fix that loop; the shape was
  wrong.
- **Fix.** One unlocked register store per edge (`gpio_port_set_bits_raw` /
  `gpio_port_clear_bits_raw`, which is what the Zephyr driver writes internally - the
  312 ns above was `pin_set`'s lock and checks, not the store), phases from a counted
  spin loop, and clock reads reduced to once per boot: a 0.5 ms calibration run makes
  the 687 ns granularity 0.1 % instead of the whole budget.
- **Measured after, on the same bench C3:** `store 187 ns, spin 44 ns/iter, t0H 407 ns,
  t1H 804 ns, 24-bit delta 10187 ns` against datasheet targets of 400 +/- 150 and
  800 +/- 150, and a predicted delta of 9600 ns for the 24-bit one-vs-zero comparison
  the boot self-test performs (+/- 1.4 us with this instrument). The self-test exists
  because there is no scope on the bench: it holds the falling-edge delay fixed and
  times a frame of ones against a frame of zeros, which is the one thing that proves
  the strip can tell them apart.
- **A correction to 2.0.3.14's entry**, which claimed the bit-bang emitted correct
  frames and that no RMT port was needed. That claim came from log lines showing
  frames alternating - evidence the software was sending, not evidence about the
  waveform - and it was wrong. The RMT HAL is present in this tree (`esp_hal_rmt` plus
  `esp32c3/rmt_ll.h` and the SoC's `rmt_struct.h`) and remains the fallback if a board
  ever cannot meet these timings in software; it was not needed here, and the reason it
  is not needed is now measured rather than argued.
- Also fixed while writing the evidence: the calibration line itself was wrong twice
  (a picoseconds-to-nanoseconds division off by 1000, then truncation), each time
  reporting `t0H == t1H` - the instrument misleading exactly when the symptom was
  being chased. The self-test's measured delta is what said the hardware was fine
  while the printed phases were not.

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
