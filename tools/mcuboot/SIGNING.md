# Signing HCP firmware images (ECDSA-P256 / MCUboot)

Runbook for the `--signed` switch: generate a key, build a verifying bootloader,
produce images it accepts, check an image without touching hardware, and know what
cannot be undone. The design rationale and the measured costs are in
[README.md](README.md); this file is the procedure.

The default build stays **unsigned** (`CONFIG_BOOT_SIGNATURE_TYPE_NONE`), so a fresh
clone needs no key material at all.

---

## 1. One minute version

```bash
cd hcp

./tools/make_signing_key.sh                      # 1st time only: keys/root-ec256.pem
./tools/build_mcuboot.sh --signed hcp_c6/esp32c6/hpcore
./build.sh hcp_c6/esp32c6/hpcore --signed        # the image you flash as the base
./tools/flash.sh hcp_c6 --app build/hcp_c6_esp32c6_hpcore_signed-root-ec256/zephyr/zephyr.signed.bin

# an update package for the server, version stamped in both places:
./build.sh hcp_c6/esp32c6/hpcore --version 0.2.0 --signed
cp build/hcp_c6_esp32c6_hpcore_v0.2.0_signed-root-ec256/zephyr/zephyr.signed.bin <server>/firmware.bin
echo 0.2.0 > <server>/version.txt
```

The two `--signed` builds must use the **same** key. `HCP_SIGN_KEY=<name>` selects
another pair (`keys/<name>.pem`, default `root-ec256`); it is also part of the build
directory name, so images signed with different keys cannot be confused.

---

## 2. The key

`tools/make_signing_key.sh [name]` generates the pair inside the build container
(no Python packages needed on the host):

```bash
./tools/make_signing_key.sh              # -> keys/root-ec256.pem  (+ .pub.c, informational)
./tools/make_signing_key.sh prod-2026    # -> keys/prod-2026.pem
```

What it does, and why each part matters:

- `imgtool keygen -t ecdsa-p256` - P-256 is what MCUboot verifies with; the file is a
  PKCS#8 private key, 241 bytes.
- `chmod 600`, and it refuses to overwrite an existing file: **losing a signing key
  means no device already in the field can ever be updated over the air again**, and
  silently regenerating one turns every image you ship into one the bootloader
  rejects.
- everything under `keys/` is gitignored. Whoever holds this file can produce
  firmware that every device you have flashed will accept - treat it as a CA key:
  offline backup, not in a repo, not in a shared drive, not the same file as
  another product.

The private key is used on both sides, in different ways: the application build signs
with it, and the MCUboot build extracts the public half from it at compile time
(`imgtool getpub`) and embeds that. So MCUboot's build also needs the file present -
that is how the two stay in sync, and also why a build machine that can sign can
build a bootloader that trusts those images.

---

## 3. Build the bootloader that verifies

```bash
./tools/build_mcuboot.sh --signed hcp_c6/esp32c6/hpcore      # also: hcp_c3, hcp_c5/esp32c5/hpcore
```

Adds `tools/mcuboot/signing.conf` (`CONFIG_BOOT_SIGNATURE_TYPE_ECDSA_P256=y`) plus a
generated fragment pointing `CONFIG_BOOT_SIGNATURE_KEY_FILE` at the key. Output:
`build/mcuboot/<board>/mcuboot.bin`. If the link reports a region overflow, the
bootloader no longer fits its partition - see the sizes table in README.md before
adding anything else to it (ESP32-C5 has the least room).

## 4. Build images it accepts

```bash
./build.sh hcp_c6/esp32c6/hpcore --signed                        # release/base image
./build.sh hcp_c6/esp32c6/hpcore --signed --version 0.2.0        # update image for the server
```

`--version` sets **both** `CONFIG_APP_VERSION` (what `hcp::ota` compares against the
server's `version.txt`) and `CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION` (the version inside
the MCUboot image header, which is what the bootloader's downgrade prevention
compares). Never set one without the other.

Signed images land in `build/<slug>_signed-<key>/zephyr/zephyr.signed.bin`. That is
the file to flash into the primary slot and the file to serve as `firmware.bin`.

A trap worth knowing: passing `CONFIG_MCUBOOT_SIGNATURE_KEY_FILE` alone is **not**
enough - `CONFIG_MCUBOOT_GENERATE_UNSIGNED_IMAGE` defaults to y when no key is known
and the key symbol `depends on` it being n, so the assignment is dropped with a
warning and the image comes out with a hash and no signature. The fragment `build.sh`
writes sets it to `n` explicitly. Check with section 5 rather than trusting the flag.

---

## 5. Check an image without hardware

`imgtool` lives in the build container (and in the mcuboot module, which the host
clone does not have on PATH), so run it the same way the build scripts do - the
repo root is mounted at /workspace:

```bash
cd ~/Documents/zephyr          # the directory that contains hcp/
docker run --rm -v "$PWD":/workspace --entrypoint /bin/bash env-zephyr-espressif -lc '
  source /opt/venv/bin/activate; cd /workspace/hcp
  I="python3 /opt/toolchains/bootloader/mcuboot/scripts/imgtool.py"
  $I verify     -k keys/root-ec256.pem build/hcp_c6_esp32c6_hpcore_v0.2.0_signed-root-ec256/zephyr/zephyr.signed.bin
  $I dumpinfo        build/hcp_c6_esp32c6_hpcore_v0.2.0_signed-root-ec256/zephyr/zephyr.signed.bin
  $I getpubhash -k keys/root-ec256.pem'
```

All three were run against real output:

- `verify` prints `Image was correctly validated`, the image version and the digest,
  and exits 0. With the wrong key it prints `No signature found for the given key`
  and exits 1. This is the check to run on anything you are about to publish.
- `dumpinfo` lists the header and the TLV area. A signed image shows **three** TLVs:

  ```
  type: SHA256 (0x10)
  type: KEYHASH (0x1)
  type: ECDSASIG (0x22)
  ```

  A hash-only image has just the first - that is what an unsigned build looks like,
  and the difference between "signed" and "I passed --signed".
- `getpubhash` prints the SHA-256 of the public key. **The KEYHASH TLV in every
  shipped image must equal this value**, because that hash is what MCUboot was built
  with. Comparing them catches "bootloader built with key A, images signed with key B"
  before a single device is reflashed.

---

## 6. Prove the device really verifies (bench, both directions)

Flashing a signed image into a signing bootloader only proves the happy path. The
rejection path is the one that matters, and it is safe to test: a rejected image is
refused *before* it ever boots, and the previous image stays in place.

1. Positive - flash the signed base (0.1.0), serve a signed 0.2.0, then
   `./tools/mqtt_watch.py pub '<sn>/cmd' 'ota http://<host>:9998/'`. Expect the
   device to come back on 0.2.0, log `new image kept (session is up)`, and still be
   on 0.2.0 after a hard reset.
2. Negative - build an image signed with a **different** key and a *higher* version,
   so neither the version gate nor a hash error can explain the outcome:

   ```bash
   ./tools/make_signing_key.sh other-ec256
   HCP_SIGN_KEY=other-ec256 ./build.sh hcp_c6/esp32c6/hpcore --version 0.9.0 --signed
   ```

   Serve it (`version.txt` = 0.9.0) and trigger the update. The application stages it
   - it does not verify anything, by design - reboots, and MCUboot refuses it. With a
   debug-logged bootloader (see README.md, "When an update does not take") the boot
   reads:

   ```
   D: bootutil_img_validate: EXPECTED_HASH_TLV == 16
   D: bootutil_img_validate: EXPECTED_KEY_TLV  == 1
   D: bootutil_img_validate: EXPECTED_SIG_TLV  == 34
   E: Image in the secondary slot is not valid!
   ```

   after which it wipes the secondary slot and boots the previous image. Watch
   `<sn>/online`: it must never leave 0.2.0.

---

## 7. What cannot be undone

- The public key is baked into `mcuboot.bin`, and **an OTA cannot update the
  bootloader**. Changing key, or going unsigned -> signed, means reflashing every
  device. Decide the key and where it is held before units leave the bench.
- **Never mix.** A signing bootloader rejects the unsigned images the default build
  produces: the device looks dead (it is booting nothing), while the ROM bootloader
  is still reachable - recover with `./tools/flash.sh` using matching artifacts.
  `flash.sh --app` / `--mcuboot` exist so the pairing is stated explicitly instead of
  "whatever was built last into build/".
- `./tools/flash.sh` erases the primary slot before writing, which also clears
  MCUboot's swap trailer; do not skip it (`--no-erase`) when moving between signed
  and unsigned images.

If the key is **lost**: field devices can only be updated by reflashing them, one by
one, with a new bootloader. If it is **leaked**: every device must be reflashed with
a new key, because an attacker can otherwise sign anything and have it accepted.
There is no revocation - no hardware key store (`MCUBOOT_HW_KEY`) and no
`SEC_CNT` security counter are enabled, and downgrade prevention is a plain version
comparison. Both are deliberate gaps for the bench, and both are decisions to make
before shipping.

---

## 8. Costs, measured

Signing adds ~5.6 KB to MCUboot (C6 44,464 B / C3 42,240 B of a 64 K partition, C5
52,512 B of 56 K = 91.6 % used) and 111 bytes per application image (KEYHASH +
ECDSA_SIG TLVs).

Per boot, measured on the rejecting boot above: hashing the 1 MB image took 1.7 s
and the signature check at most 2 ms (timestamp of `EXPECTED_SIG_TLV` to the
verdict). The hash is not new - MCUboot already hashes the primary slot every boot
with `CONFIG_BOOT_VALIDATE_SLOT0=y`, which is what makes a rejected update take a
second or two rather than a few milliseconds. The table in [README.md](README.md)
has the details.
