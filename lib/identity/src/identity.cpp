/*
 * hcp::identity - pure string helpers (OS-independent, host-tested).
 * SPDX-License-Identifier: Apache-2.0
 */
#include "hcp/identity/identity.h"

#include <string.h>

namespace hcp {
namespace identity {
namespace {

constexpr char kHex[] = "0123456789abcdef";
// RFC 1035/1123: one DNS label is at most 63 characters.
constexpr size_t kLabelMax = 63;

bool IsLabelChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
}

char Lower(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
}

}  // namespace

bool FormatFromMac(const uint8_t mac[6], const char* prefix, char* out,
                   size_t cap) {
  if (mac == nullptr || prefix == nullptr || out == nullptr) {
    return false;
  }
  const size_t plen = strlen(prefix);
  if (plen + 12 + 1 > cap) {
    return false;
  }
  memcpy(out, prefix, plen);
  size_t o = plen;
  for (int i = 0; i < 6; i++) {
    out[o++] = kHex[mac[i] >> 4];
    out[o++] = kHex[mac[i] & 0x0F];
  }
  out[o] = '\0';
  return true;
}

bool ComposeSerial(const char* code, const char* prefix, char* out, size_t cap) {
  if (code == nullptr || code[0] == '\0' || out == nullptr || cap < 2) {
    return false;
  }
  const size_t clen = strlen(code);
  const size_t plen = (prefix != nullptr) ? strlen(prefix) : 0;
  // A factory blob that already carries the prefix must not be doubled.
  const bool has_prefix =
      plen > 0 && clen >= plen && memcmp(code, prefix, plen) == 0;
  const size_t total = (has_prefix ? 0 : plen) + clen;
  if (total + 1 > cap) {
    return false;
  }
  size_t o = 0;
  if (!has_prefix) {
    memcpy(out, prefix, plen);
    o = plen;
  }
  memcpy(out + o, code, clen);
  out[o + clen] = '\0';
  return true;
}

bool ParseProdinfo(const uint8_t* blob, size_t len, const char* prefix, char* out,
                   size_t cap) {
  if (blob == nullptr || out == nullptr || len < kProdinfoHeader) {
    return false;
  }
  if (memcmp(blob, kProdinfoMagic, sizeof(kProdinfoMagic)) != 0) {
    return false;  // erased, or a partition this build does not know
  }
  const size_t code_len =
      static_cast<size_t>(blob[4]) | (static_cast<size_t>(blob[5]) << 8);
  if (code_len == 0 || kProdinfoHeader + code_len > len ||
      code_len + 1 > kSerialMax) {
    return false;
  }
  char code[kSerialMax];
  for (size_t i = 0; i < code_len; i++) {
    const uint8_t c = blob[kProdinfoHeader + i];
    // A NUL or an erased (0xFF) byte inside the code means the write did not
    // complete; better falling back to the MAC than serving a half value.
    if (c == 0x00 || c == 0xFF) {
      return false;
    }
    code[i] = static_cast<char>(c);
  }
  code[code_len] = '\0';
  return ComposeSerial(code, prefix, out, cap);
}

bool FormatHostname(const char* sn, char* out, size_t cap) {
  if (sn == nullptr || out == nullptr || cap < 2) {
    return false;
  }
  const size_t limit = (cap - 1 < kLabelMax) ? cap - 1 : kLabelMax;
  size_t o = 0;
  for (const char* p = sn; *p != '\0' && o < limit; p++) {
    const char c = Lower(*p);
    out[o++] = IsLabelChar(c) ? c : '-';
  }
  // Neither end of a label may be '-'; interior ones are fine.
  while (o > 0 && out[o - 1] == '-') {
    o--;
  }
  size_t lead = 0;
  while (lead < o && out[lead] == '-') {
    lead++;
  }
  if (lead > 0) {
    memmove(out, out + lead, o - lead);
    o -= lead;
  }
  out[o] = '\0';
  return o > 0;
}

}  // namespace identity
}  // namespace hcp
