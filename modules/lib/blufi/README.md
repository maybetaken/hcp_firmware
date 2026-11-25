# blufi — ESP BluFi compatible BLE provisioning for Zephyr

Implements the provisioning protocol Espressif's apps speak (the "BluFi" GATT
service): a self-contained frame codec, the BLE GATT transport on Zephyr, and the
optional key-negotiation + payload encryption used when the phone asks for it.
Nothing here depends on a particular product, board or radio beyond what Zephyr's
`BT_ESP32` controller provides.

```
frame.c / include/blufi/frame.h        frame layout, CRC16, protobuf-ish fields
blufi_ble.c / include/blufi/blufi.h    GATT service, advertise/connect, subtypes
include/blufi/blufi_security.h         negotiation + AES-CFB128 (optional)
sec/                                   the security handshake (see below)
deps/mbedtls                           Mbed TLS submodule, pinned by its gitlink
```

## Using it

`CONFIG_BLUFI=y` gives you provisioning over **plaintext** transport, which is what
both the Espressif apps and the WeChat mini-program use by default. The module
selects the BT symbols it needs (`BT`, `BT_PERIPHERAL`, `BT_DRIVERS`, `BT_ESP32`);
the application owns advertising names, the Wi-Fi side and what to do with the
credentials it receives.

Note two things the stack defaults get wrong for BluFi, and that this module
therefore sets explicitly in its examples: the ATT/L2CAP MTU (BluFi control frames
are far larger than the 23-byte default, so notifications fail with `-ENOMEM`
without `BT_L2CAP_TX_MTU`/`BT_BUF_ACL_*_SIZE` raised) and `BT_ID_MAX` if you
advertise from a fresh identity each boot.

## Encrypted provisioning: `CONFIG_BLUFI_SECURITY`

Off by default. When on, the phone can negotiate DH + AES-128-CFB128 with
MD5-derived key material, as the ESP-IDF example does. The module implements the
handshake (`sec/blufi_security.c`) but **no crypto of its own**: `dhm`, `bignum`,
`constant_time`, `aes` and `md5` are compiled out of an Mbed TLS checkout this
library carries as a git submodule - upstream files, unmodified. Mbed TLS 2.28 is
the last series with classic finite-field DH: TF-PSA-Crypto and Mbed TLS 3.x/4.x
removed it, and their PSA FFDH knows only the RFC 7919 named groups, while BluFi
negotiates its own 256-bit group.

```bash
# once, after cloning (or clone with --recurse-submodules):
git submodule update --init --depth 1 deps/mbedtls
```

Then build with `CONFIG_BLUFI_SECURITY=y`; CMake prints which tree it resolved to.
It looks in three places, in order: `-DBLUFI_MBEDTLS_DIR=<path>` if you set it, then
`modules/crypto/mbedtls-2.28` in the west workspace, then `deps/mbedtls` here. The
west entry is for a workspace that already provides Mbed TLS 2.28: a sibling
feature needing the same tree gets it from there, so neither module reaches into
the other and that workspace never runs `submodule update`. Taken on its own, this
library's submodule is the path, and it stays.

`sec/mbedtls/CMakeLists.txt` builds those upstream files as a library of their
own, for two reasons: the application already links an AES, so
`blufi_mbedtls_rename.h` renames the AES symbols at compile time (no upstream file
is edited), and `blufi_dhm_compat.h` keeps those translation units off the
application-wide `MBEDTLS_CONFIG_FILE`. Cost on ESP32-C6: 63 functions, about
10.8 KB of text.

Honest note on strength: a 256-bit DH group and MD5-derived keys are legacy
compatibility, not security. If you can choose, provision over plaintext BluFi
into WPA2/WPA3 and rely on the TLS/signed-image layers elsewhere.

## License

Apache-2.0 - see [LICENSE](LICENSE). It covers this repository's files only. Mbed TLS is
submoduled, not vendored, and stays Apache-2.0 (dual licensed GPL-2.0-or-later)
under its own terms; a binary that enables encrypted provisioning therefore carries
their notices. Bump the pin with an ordinary `git submodule` update - the gitlink
commit is the record of exactly which upstream code went in, and `git -C
 deps/mbedtls log -1` answers "what version is this" without anyone reading a
script.
