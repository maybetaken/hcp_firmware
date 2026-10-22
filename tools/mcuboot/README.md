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

## Not done yet (deliberate)

* **Signing.** `CONFIG_BOOT_SIGNATURE_TYPE_NONE` in `mcuboot.conf`. Switching means
  generating a key once (`imgtool -k root-ec256-pub.pem key gen --ecc256`), keeping
  the private key out of the repo, and setting
  `CONFIG_BOOT_SIGNATURE_TYPE_ECDSA_P256` + `CONFIG_BOOT_SIGNATURE_KEY_FILE`
  on *both* sides. Decide before any device ships: the public key is baked into
  MCUboot, which no OTA can replace.
* **TLS.** The HTTP source returns `-ENOTSUP` for `https://`.
* **Per-board bring-up.** Only the C6 has been run on hardware; C3/C5 need the same
  two commands and a look at the log levels (`boot/zephyr/socs/` has no `esp32c5`
  fragment, so `mcuboot.conf` is the whole configuration there).
