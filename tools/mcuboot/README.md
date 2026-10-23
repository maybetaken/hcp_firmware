# MCUboot + OTA on the HCP boards

How the firmware update path is built, flashed and tested. The update logic itself
lives in the reusable [`modules/lib/ota_client`](../../modules/lib/ota_client/README.md)
module; [`lib/ota`](../../lib/ota) is the ~200-line product wiring around it.

## Layout (comes from the board devicetree, not from this file)

Both the application and MCUboot read their slots from
`boards/helios/<board>/*.dts`, which includes Espressif's default partition table
plus the 4 KB `prodinfo` slice carved off the tail of `storage`.

| | hcp_c6 / hcp_c3 (4 MB table) | hcp_c5 (8 MB table) |
| --- | --- | --- |
| MCUboot | `0x000000`, 64 K | `0x002000`, 56 K |
| sys | `0x010000`, 64 K | `0x010000`, 64 K |
| image-0 (primary, the app) | `0x020000`, 1792 K | `0x020000`, 3840 K |
| image-1 (secondary, staging) | `0x1e0000`, 1792 K | `0x3e0000`, 3840 K |
| storage (settings/NVS) | `0x3b0000`, 188 K | `0x7b0000`, 188 K |
| **prodinfo** (factory SN, read-only) | `0x3df000`, 4 K | `0x7df000`, 4 K |
| image-scratch | `0x3e0000`, 124 K | `0x7e0000`, 124 K |

`storage` keeps its original start address, which is why reflashing - or the whole
OTA mechanism - never touches the Wi-Fi credentials or the burned serial number.

## Build

```bash
./tools/build_mcuboot.sh hcp_c6/esp32c6/hpcore   # -> build/mcuboot/hcp_c6_esp32c6_hpcore/mcuboot.bin
./build.sh hcp_c6/esp32c6/hpcore                # app; zephyr.signed.bin is what goes in image-0
```

The app is linked into image-0 automatically: `CONFIG_OTA_CLIENT_SINK_MCUBOOT`
selects `BOOTLOADER_MCUBOOT`, which pulls in `soc/espressif/<soc>/mcuboot.ld` and
sets `CONFIG_FLASH_LOAD_OFFSET` to the primary slot. `west build` then runs
`imgtool` for you, producing `zephyr.signed.bin` - flash that, not `zephyr.bin`.

Keep the two version numbers in step:

| knob | where | meaning |
| --- | --- | --- |
| `CONFIG_APP_VERSION` | app | what hcp::ota compares against the server's `version.txt` |
| `CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION` | app | the version in the image header; MCUboot's own downgrade prevention compares *this* |

`./build.sh <board> --version 0.2.0` sets both at once and builds into
`build/<slug>_v0.2.0`, next to the plain build, so a release image and the update
image can be produced one after the other. Without the flag the header keeps
Zephyr's default `0.0.0+0` - a lie MCUboot believes: downgrade prevention then
compares against 0.0.0, and the image cannot be flashed back over a newer one.
The header version is visible in the bootloader's `Image version:` line and in
`boot_version_cmp`, so it is easy to check.

## Flash

```bash
./tools/flash.sh hcp_c6            # host side: mcuboot @boot addr + signed app @0x20000
```

**Read this before flashing by hand.** MCUboot keeps its swap state in the
*trailer at the end of the slot*, not in the image bytes. `esptool write_flash`
only erases the range it writes, so writing a fresh application over image-0
leaves an old "this is a test image, revert it" trailer in place. With the
secondary slot empty, MCUboot then finds nothing to revert to and boots
nothing - the board stays silent on the console while still being perfectly
reachable through the ROM bootloader. `tools/flash.sh` erases the primary slot
before writing; the manual equivalent is:

```bash
esptool --chip esp32c6 -p /dev/cu.usbmodem101 erase_region 0x20000 0x1c0000
esptool ... write_flash 0x000000 build/mcuboot/.../mcuboot.bin \
                    0x020000 build/hcp_c6_esp32c6_hpcore/zephyr/zephyr.signed.bin
```

## Update flow on the device

`hcp::ota` runs on its own thread and is triggered from the command topic:

```bash
./tools/mqtt_watch.py sub  'hcp-M1230/#'
./tools/mqtt_watch.py pub  'hcp-M1230/cmd' 'ota http://192.168.31.74:9998/'
# or, using the host Home Assistant announced over SSDP:  'ota'
```

It expects `version.txt` (one dotted-numeric line) and `firmware.bin` (the signed
image) in that directory - the same pair the ESP-IDF firmware used, served on port
9998 by default. Locally `python3 -m http.server 9998 -d <dir>` is enough.

What happens next, in the order the code enforces:

1. `version.txt` is fetched and compared with `CONFIG_APP_VERSION`; an equal or
   older version stops here with `-EALREADY` (a normal outcome, not a failure).
2. The secondary slot is **erased**, then the image is streamed into it through
   `flash_img`. The erase is not optional - see the module README - and it also
   clears any trailer left by an earlier swap, which is what makes a reflowed
   board and a board that has already updated once behave the same.
   A transfer that ends short of `Content-Length` is refused.
3. `boot_request_upgrade(false)` asks MCUboot to swap on the next reset, and the
   device reboots.
4. The new image boots and is now a trial: MCUboot will revert on the next reset
   unless it is confirmed. `hcp::ota` confirms only after
   `CONFIG_HCP_OTA_CONFIRM_AFTER_MS` of uptime *and* a live broker session, so a
   firmware that cannot reach the network is rolled back instead of kept.

Budget **about a minute and a half per update** on the C6: a few seconds to erase,
~7 s to download 1 MB, then 70-75 s for MCUboot's swap (it moves the primary slot
through the scratch area one 4 KB sector at a time). The console is silent for all
of that with the shipped `mcuboot.conf`, which is not a hang.

### The trial deadline (watchdog)

"Revert on the next reset" is not much use if nobody ever resets: an image that
boots and then hangs, or that boots and cannot reach the network, would sit there.
So both sides carry a deadline, on the same MWDT0 (`watchdog0` alias):

| who | when | installed window | chip actually resets at |
| --- | --- | ---: | ---: |
| MCUboot (`BOOT_WATCHDOG_TIMEOUT_MS`) | every boot, before jumping to the app | 600000 ms | ~20 min, and the app replaces/cancels it within milliseconds |
| hcp::ota (`CONFIG_HCP_OTA_WATCHDOG_MS`) | first thing in `main()`, only if this boot is a trial | 300000 ms | ~10 min |

The application's first act is therefore: **trial → re-arm tighter and never feed;
final image → cancel outright**. Cancelling is not optional - a bootloader that
arms and an application that never disarms resets a perfectly healthy device once
per window, so MCUboot and the application have to be reflashed together.
Confirming the image (`new image kept (session is up)`) disarms it too.

Two measured details worth knowing before changing the numbers:

- Espressif's MWDT is **two-stage**: the installed window passes once as an
  interrupt and again before the reset, so the chip comes back at 2x what was
  written into the register. Installed 20000 ms on a C6 measured a reset at 40.0 s,
  with the ROM banner reporting `rst:0x7 (TG0_WDT_HPSYS)`.
- The bootloader has to feed the running deadline through its own ~75 s of flash
  work (`CONFIG_BOOT_WATCHDOG_FEED=y`), otherwise the rollback it is performing
  gets interrupted by the very reset it is waiting for. Note that `FEED` without
  `SETUP_AT_BOOT` does not even compile in this MCUboot (its feed path uses a
  channel id that is only declared when a timeout is installed).

Bench result, both directions: trial 0.2.0 with a 20 s window reached the broker
late, reset itself at 40.0 s, MCUboot moved 0.1.0 back (72 s), and the device was
healthy on 0.1.0 again with nobody touching it; a final image booted under a
bootloader deadline of 20 s and ran 150 s+ without a single reset, which is the
cancel path working.

Verified on an ESP32-C6 at 02:0x (bench): 0.1.0 -> 0.2.0 -> 0.3.0 in a row, each
step `new image kept (session is up)` at the confirmation window and still there
after a hard reset; the 0.2.0 step was deliberately run with the secondary slot
full of `0x00` first, which is the case that used to fail.

## When an update does not take

The shipped MCUboot configuration logs nothing of its own (`CONFIG_LOG=y` but the
module level is OFF; the `I (soc_init)` lines come from the ESP HAL). To see the
bootloader's reasoning - which magic it found in which trailer, whether the image
hashed, what it decided - build a second bootloader with its debug output on:

```bash
cat > build/tmp/mcuboot_dbg.conf <<'EOF'
CONFIG_MCUBOOT_LOG_LEVEL_DBG=y
CONFIG_CONSOLE=y
CONFIG_UART_CONSOLE=y
EOF
docker run --rm -v "$PWD/..":/workspace --entrypoint /bin/bash env-zephyr-espressif -lc \
  "source /opt/venv/bin/activate; export ZEPHYR_BASE=/opt/toolchains/zephyr; \
   west build -p always -d /workspace/hcp/build/mcuboot_dbg -b hcp_c6/esp32c6/hpcore \
     /opt/toolchains/bootloader/mcuboot/boot/zephyr -- -DBOARD_ROOT=/workspace/hcp \
     -DEXTRA_CONF_FILE='/workspace/hcp/tools/mcuboot/mcuboot.conf;/workspace/hcp/build/tmp/mcuboot_dbg.conf'"
esptool --chip esp32c6 -p /dev/cu.usbmodem101 write_flash 0x0 build/mcuboot_dbg/zephyr/zephyr.bin
```

Typical verdicts, both seen on this bench:

* `Image in the secondary slot is not valid!` followed by an erase of the whole
  slot and `Swap type: none` - the staged bytes did not match their own hash TLV.
  Almost always a slot that was written without being erased.
* `magic=unset ... Swap type: none` with no erase at all - the application never
  asked for the swap; check that `boot_request_upgrade()` was reached and that the
  primary slot's trailer was not wiped by a reflash (`tools/flash.sh`).

## Signing (ECDSA-P256), opt-in

Unsigned (`CONFIG_BOOT_SIGNATURE_TYPE_NONE`) remains the default so a fresh clone
builds without key material. The procedure - generating a key, building both halves
with `--signed`, checking an image without hardware, the bench test that proves the
bootloader really verifies, and what cannot be undone afterwards - is in
**[SIGNING.md](SIGNING.md)**.

What the flag changes in the image: `imgtool` adds a KEYHASH TLV and an `ECDSA_SIG`
TLV next to the SHA-256 it always wrote (+111 bytes):

```
TLV 0x10 SHA256(hdr+body) len 32   TLV 0x01 KEYHASH len 32   TLV 0x22 ECDSA_SIG len 71
```

Measured cost in the bootloader (its own partition is 64 K on C3/C6, 56 K on C5):

| board | mcuboot.bin unsigned | signed | partition | headroom after signing |
| --- | ---: | ---: | ---: | ---: |
| hcp_c6 | 38,864 | 44,464 | 65,536 | 21,072 |
| hcp_c3 | 36,688 | 42,240 | 65,536 | 23,296 |
| hcp_c5 | 46,944 | 52,512 | 57,344 | **4,832 (91.6 % used)** |

So ~5.6 KB for verification on all three; C5 is tight enough that anything else added
to MCUboot there needs checking against that 4.8 KB.

Bench evidence, both directions. Positive: signed 0.1.0 -> signed 0.2.0 over the air,
confirmed, hard reset, still 0.2.0. Negative: an image signed with a **different key**,
version 0.9.0 (so above what was running) and with a perfectly consistent hash TLV,
was staged by the application and then refused by the bootloader -
`EXPECTED_HASH_TLV`/`EXPECTED_KEY_TLV`/`EXPECTED_SIG_TLV` all found,
`E: Image in the secondary slot is not valid!`, secondary slot wiped, 0.2.0 booted.
That is the point: the application does not verify anything, the bootloader does.

Two traps, both silent, are written out in SIGNING.md: `CONFIG_MCUBOOT_GENERATE_UNSIGNED_IMAGE`
must be forced to n or the key assignment is dropped without an error, and signed /
unsigned artifacts must never be mixed because the trust anchor lives in a
bootloader that no OTA can replace.

Key handling (`keys/` gitignored, CA-grade secret, what losing or leaking it means)
and the verification commands (`imgtool verify` / `dumpinfo` / `getpubhash`) are in
**[SIGNING.md](SIGNING.md)**.

What signing does *not* buy: no TLS (the download is still plaintext - which is now
harmless rather than dangerous, since an attacker's image is refused by the
bootloader), and no revocation. The trust anchor is one embedded public key, and
neither MCUboot's hardware-key option nor the security-counter TLV is enabled, so a
compromised key means reflashing and "downgrade prevention" is a plain version-number
comparison.

## Not done yet (deliberate)

* **Key rotation / anti-rollback counter.** See the paragraph above: one embedded
  public key, no `SEC_CNT` in the image, so there is no way to retire a key or pin a
  minimum version in hardware. Both have to be decided before units ship, because
  neither can be introduced by an OTA.
* **Signing on by default.** `mcuboot.conf` still says NONE and every `--signed`
  decision is per-build; turning it into the default is a one-line change plus
  deciding where the key lives (see `CONFIG_BOOT_SIGNATURE_TYPE_ECDSA_P256` in
  `tools/mcuboot/signing.conf`).
* **TLS.** The HTTP source returns `-ENOTSUP` for `https://`.
* **Per-board bring-up.** Only the C6 has been run on hardware; C3/C5 need the same
  two commands and a look at the log levels (`boot/zephyr/socs/` has no `esp32c5`
  fragment, so `mcuboot.conf` is the whole configuration there). C5 additionally has
  only 4.8 KB of MCUboot partition left after signing.
