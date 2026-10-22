/*
 * hcp::identity - one serial number for the whole device.
 *
 * The same string names this device everywhere it is seen from the outside: the
 * MQTT client-id and topic prefix ("<sn>/online", "<sn>/cmd", ...), and the
 * hostname a router shows in its client list. Keeping it in one place matters
 * because the value is not always derived locally: a provisioning step (factory
 * write, or an earlier boot) can put an arbitrary serial number in storage, and
 * the network identity then has to follow that instead of the MAC.
 *
 * Resolution order: read-only "prodinfo" flash partition (burned per device by
 * tools/make_sn_bin.py) -> storage -> station MAC ("<prefix><12 hex digits>",
 * stored so it never changes again) -> "<prefix>pending" when even the MAC is
 * unavailable.
 *
 * Google C++ Style naming. SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace hcp {
namespace identity {

// Storage key holding the serial number. Deliberately the key hcp::mqtt has used
// since the first MQTT build, so already-provisioned devices keep their identity.
constexpr const char* kSerialKey = "dev/snr";

// Longest serial number (plus NUL). Also bounds the "<sn>/" MQTT topic prefix.
constexpr size_t kSerialMax = 33;

// Pure: "<prefix>" plus the MAC as 12 lowercase hex digits. Returns false when
// the result would not fit (out is then left untouched).
bool FormatFromMac(const uint8_t mac[6], const char* prefix, char* out, size_t cap);

// Pure: "<prefix><code>", except that a code which already starts with the
// prefix is taken as-is - burning "M1230" and burning "hcp-M1230" both end up as
// hcp-M1230, which is what a factory tool that does not know the build's prefix
// needs. Returns false when the result would not fit or the code is empty.
bool ComposeSerial(const char* code, const char* prefix, char* out, size_t cap);

// Layout of the "prodinfo" partition, written by tools/make_sn_bin.py and read
// through the flash map. Little-endian, 0xFF where unwritten (erase state):
//   0  magic 'H','C','P','1'
//   4  u16 length of the serial code
//   6  the code, raw ASCII (no NUL on flash)
constexpr char kProdinfoMagic[4] = {'H', 'C', 'P', '1'};
constexpr size_t kProdinfoHeader = 6;

// Pure: validate a prodinfo blob and compose the serial from it. False on a bad
// magic, a length that runs past the buffer, embedded NUL/0xFF (torn write) or a
// result that does not fit - never a partially trusted value.
bool ParseProdinfo(const uint8_t* blob, size_t len, const char* prefix, char* out,
                   size_t cap);

// Pure: turn a serial number into a hostname a DHCP client may announce, i.e. an
// RFC 1123 label: lowercase, only [a-z0-9-], no leading or trailing '-'. Anything
// else becomes '-'; the result is truncated to one label. Returns false when
// nothing usable comes out or the buffer is too small. Worth the bother because a
// router that receives a malformed option 12 just ignores it and falls back to
// the MAC vendor - the "my device is called espressif again" symptom.
bool FormatHostname(const char* sn, char* out, size_t cap);

// Resolve the serial number and announce it as the device hostname. Call once at
// boot, after hcp::net::wifi::Init() and before the first Wi-Fi connect: DHCP
// carries the name in its DISCOVER, so a later change only shows up on a renew.
// Idempotent. Returns false when no durable serial could be produced yet (no
// station MAC and/or no storage) and only the "<prefix>pending" placeholder is
// available - the hostname is still announced, and the real number is derived on
// the next boot.
bool Init();

// The resolved serial number; empty string before Init() has run. Stable for the
// lifetime of the program, so callers may keep the pointer.
const char* serial_number();

}  // namespace identity
}  // namespace hcp
