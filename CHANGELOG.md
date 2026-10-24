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

## 2.0.3.19 — 2026-10-25

One LED route for all three boards: the C5 and the C6 stop driving the strip over
`worldsemi,ws2812-spi`, and the SPI2 pin groups disappear from every board's pinctrl
with it.

The reason is the second pin. The WS2812 has one wire; a SPI controller insists on
driving a clock as well, and that pad was never free: on the C3 it landed on GPIO11,
which is the chip's own RX (two boards dark before it moved to GPIO7), and on the C6 it
sat on **GPIO6, which is that board's button**. The C5's GPIO9 was free only because it
was picked by exclusion. RMT needs the data line and nothing else, so three boards now
share one devicetree file and no board spends a pad on a signal nobody reads.

Porting the driver to the other two parts turned up three differences, each read out of
the HAL rather than assumed from the C3:

| | ESP32-C3 | ESP32-C5 / C6 |
|---|---|---|
| symbol push register | `data_ch[ch]` | `chndata[ch].chndata` (still 32 bits: one write, one symbol) |
| per-channel readback | `tx_conf[] / tx_status[] / tx_lim[]` | `chnconf0[] / chnstatus[] / chn_tx_lim[]` |
| counter clock source | `RMT_CLK_SRC_APB` | `RMT_CLK_SRC_PLL_F80M` |

Both clocks are 80 MHz, which is the only reason the tick arithmetic (32/64/100 ticks of
12.5 ns) did not become per-board. The channel bound comes from the per-channel *config*
array because the headers disagree about the data array (the C5 declares `chndata[4]`,
`chnstatus[2]`); the resulting 2 also matches the SoC signal table, which lists TX
signals only for channels 0 and 1.

`Strip::Show()` also loses its `irq_lock()`. That blackout existed for the SPI route, where
a gap of tens of microseconds inside a frame is read as the latch condition — the C6's
stray green flash while violet was intended. RMT owns the waveform once started, so an
ISR delays a frame rather than corrupting a bit in one, and keeping the lock would have
meant running the driver's busy-wait — and its 200000-pass timeout — with interrupts off.

**What this release does not do: the C6's strip still does not light.** The first run of
this driver on C6 hardware says so plainly — `rmt ch0 never done` on every frame, with
`limit=0x00000017` (correct), `chnstatus=0x00018000` and no done bit. Decoding that word
against the C6 header is the whole diagnosis: bits [20:12] are the *APB write* address on
this generation (24 symbols pushed, all of them) while the *read* address in [8:0] stayed
at zero — the symbols reached the channel RAM and the counter never ran on them. The
omission is the counter clock's own enable, `PCR.rmt_sclk_conf.rmt_sclk_en`, which the
C5/C6 keep outside the RMT registers and which this driver never touched; the C3 gates it
differently and worked regardless. Fixed in 2.0.3.20.

Also measured here, because it is the point of the bench work: the C6 went from 0.3.5 to
2.0.3.19 **over the network**, with no USB beyond a console capture — MCUboot is on it
(its banner reads `MCUboot 2nd stage bootloader`), the swap ran, and the device came back
announcing the new version.

## 2.0.3.18 — 2026-10-24

The strip is driven now. Two more causes, both invisible from the completion bit that
the previous two releases had been treating as proof:

- **The symbols never entered the channel RAM.** In non-FIFO mode the write pointer
  stayed at zero through all 24 DATA writes, so the counter ran over empty memory,
  reached its limit and reported done — a successful transfer of nothing. The FIFO push
  port is the right mode here: `tx_status` went from `0x00000000` to `0x00018000`
  across the same writes.
- **Carrier modulation is asserted in the channel's reset value** (`tx_conf` read back
  `0x00710140`), and a modulated output with the carrier registers at reset is a flat
  line on the pin. Disabled as IDF's own driver does in its channel reset; `tx_conf`
  is now `0x00110140` after init.

The lesson, since it is the third time this session that "the driver reported success"
was not evidence about the hardware: a peripheral's completion flag says its state
machine finished, and nothing else. Whether data arrived and whether the pin moved are
separate facts, so the driver now prints them — channel status after the symbols are
written and after completion — once per boot, and the first-frame measurement is at
INF because two lines per boot is cheap next to a guess. One of those measurements also
caught a flaw in itself: `int_raw` was being read after the status clear, which prints
zero no matter what happened.

Bench C3: 24 symbols, write pointer advanced, done with no error bit, zero failures
across a window that included the 2 Hz blink.

## 2.0.3.17 — 2026-10-24

> Correction, same day: this release fixed the reset gate (below) but **still did not
> drive the strip** — the symbols never reached the channel RAM and the carrier was
> modulating the output flat. See 2.0.3.18. What follows is accurate about the four
> register facts it names, and those were real; they were not sufficient.

The RMT driver now actually transmits; on the bench it went from "every frame fails"
to zero failures over a window that included the 2 Hz blink, and the four things that
were wrong are worth naming because each was found by measurement, not by reading.

- **The block was held in reset.** `rmt_ll_enable_bus_clock()` on its own is not
  enough on the ESP32-C3: every RMT register read back zero — including the ones
  written moments earlier — so no configuration could take. `rmt_ll_reset_register(0)`
  (assert, then de-assert) fixed it. This is what "the LED is dead" looked like from
  the driver's side, and it is invisible from any register dump that isn't asked.
- **The completion bit was read from a gated register.** Polling
  `rmt_ll_tx_get_interrupt_status()` can never succeed in a driver that takes no
  interrupt: it reads `int_st`, which is *raw AND interrupt-enabled*. The raw variant
  is the one to poll, and it also lets the error bit be distinguished from done.
- **`tx_lim` is the end of this chunk, not the channel's capacity.** Left at 47 while
  24 symbols were written, the read pointer ran into symbols that never arrived and
  the frame never ended. Set per flush from the symbol count.
- **RMT's symbol RAM has its own power switch,** and leaving it to the PMU let the
  registers answer while the memory was off. Forced on: this device never suspends.
- Kept: on the rare timeout the driver prints `tx_conf/limit/status/raw/sys_conf`
  plus the clock-enable and reset-register words, because the difference between
  "every register is zero" (not clocked / in reset) and "non-zero but no done" (symbol
  path or limit) is one line to read and cost four flash cycles to discover.

## 2.0.3.16 — 2026-10-24

> Correction, found the same day: this release's driver **did not make the LED work**.
> It compiled, bound and ran, but every frame failed with "never reported done" (see
> 2.0.3.17 for the four reasons). What below describes as done is the structure - the
> module, the binding, the removal of the bit-bang branch - not a working strip.

The C3's WS2812 is driven by a real driver now: `modules/drivers/ws2812_rmt`, a
Zephyr module implementing the standard `led_strip` API behind a devicetree
compatible (`espressif,esp32-ws2812-rmt`), in the same shape as `modules/lib/blufi`.
`lib/led` went back to one code path — the `hcp-led-strip` alias — so the application
no longer knows or cares which peripheral produces the waveforms.

- **Why a driver and not a branch.** The previous fix attempt put bit-bang code inside
  `lib/led` behind a `CONFIG_HCP_LED_BACKEND_GPIO_BITBANG` #ifdef. That measured
  in-spec phases (t0H 407 ns, t1H 804 ns) and the strip still took nothing, because
  the loop can only be as accurate as the primitives it times itself with — a GPIO
  store costs 312 ns and a system-clock read 687 ns against a 400 ns phase. RMT counts
  ticks off a hardware clock, so the phase widths do not depend on the CPU at all, and
  the frame needs no interrupt blackout (which the SPI route does need, and still
  keeps). Dead bit-bang code that referenced a devicetree node no board provides was
  removed rather than left compiling-by-accident.
- **The module wiring needed two things that are not obvious.** `ZEPHYR_EXTRA_MODULES`
  brings in a module's CMakeLists and Kconfig but *not* its devicetree bindings —
  those are collected from the west manifest, and this application is not a west
  module — so the app also lists the module in `DTS_ROOT` (documented in
  `pre_dt.cmake`). Without that the node's properties were silently ignored, no
  `DT_HAS_*` symbol existed, and the failure surfaced much later as an undefined
  device object at link time. And `LED_DEVICE_DT_INST_DEFINE` does not exist in this
  Zephyr; the in-tree `ws2812_spi.c` uses plain `DEVICE_DT_INST_DEFINE`.
- **The GPIO-matrix signal index comes from the SoC's own table**
  (`esp_hal_rmt/<soc>/rmt_periph.c`, compiled in) rather than from arithmetic on
  `RMT_SIG_OUT0_IDX`. "The channel signals are contiguous" is the kind of reasonable
  assumption whose symptom is an LED that never answers.
- **`color-mapping` is honoured, not hardcoded.** The shared `led-strip.yaml` binding
  requires it, so the driver reads it and rejects a repeated or non-RGB id at init — a
  wrong order otherwise shows wrong colours while looking perfectly healthy.
- **ESP32-C5 and C6 stay on the SPI route, on purpose.** Their RMT is a later hardware
  generation: the channel-data member is `chndata[]` rather than the C3's `data_ch[]`
  (4 channels per instance on the C5, 2 on the C6). Making that compile is trivial and
  cannot be verified without one of those boards on the bench, and a driver writing the
  wrong generation's register reports itself as a dead LED — the exact symptom this
  started from. So the driver `#error`s for any part it does not describe, and
  `hcp_ws2812_rmt.dtsi` says so in its header. The C6's LED is verified working on SPI;
  it was not going to be traded for an unverified change on a board that is not here.
- Also corrected while here: `boards/helios/hcp_c3/hcp_c3-pinctrl.dtsi`'s comment
  claimed GPIO7 is the LED's SPI clock, which stopped being true the moment the C3
  moved to RMT. It now says the group is applied by nothing and keeps the reason the
  clock was moved off GPIO11 (that pin is the chip's own RX; two boards were dark).

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
