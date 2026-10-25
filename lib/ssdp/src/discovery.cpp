/*
 * hcp::ssdp - announcement parsing (pure, host-tested).
 * Copyright (c) 2026 maybetaken
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "hcp/ssdp/discovery.h"

#include <stdlib.h>
#include <string.h>

namespace hcp {
namespace ssdp {
namespace {

constexpr uint16_t kMqttDefaultPort = 1883;

bool IsBlank(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/* Trim both ends into out; false when the remainder is empty or too long. */
bool TrimTo(const char* s, size_t len, char* out, size_t cap) {
  size_t begin = 0;
  size_t end = len;
  while (begin < end && IsBlank(s[begin])) {
    begin++;
  }
  while (end > begin && IsBlank(s[end - 1])) {
    end--;
  }
  const size_t n = end - begin;
  if (n == 0 || n + 1 > cap) {
    return false;
  }
  memcpy(out, s + begin, n);
  out[n] = '\0';
  return true;
}

}  // namespace

bool ParseAnnouncement(const char* data, size_t len, char* url_out, size_t url_cap) {
  if (data == nullptr || url_out == nullptr || len == 0 || url_cap == 0) {
    return false;
  }
  url_out[0] = '\0';

  const size_t marker_len = sizeof(kMarker) - 1;
  if (len < marker_len) {
    return false;
  }
  for (size_t i = 0; i + marker_len <= len; i++) {
    if (memcmp(data + i, kMarker, marker_len) == 0) {
      return TrimTo(data + i + marker_len, len - i - marker_len, url_out, url_cap);
    }
  }
  return false;
}

bool ParseMqttUrl(const char* url, char* host, size_t host_cap, uint16_t* port) {
  static const char kScheme[] = "mqtt://";
  const size_t scheme_len = sizeof(kScheme) - 1;
  if (url == nullptr || host == nullptr || port == nullptr) {
    return false;
  }
  *port = kMqttDefaultPort;
  if (strncmp(url, kScheme, scheme_len) != 0) {
    return false;
  }

  const char* rest = url + scheme_len;
  const char* colon = strchr(rest, ':');
  const size_t host_len = colon != nullptr ? static_cast<size_t>(colon - rest)
                                           : strlen(rest);
  if (host_len == 0 || host_len + 1 > host_cap) {
    return false;
  }
  // Only host[:port] is supported; a path or query would mean the announcement
  // is not a broker address at all.
  if (memchr(rest, '/', host_len) != nullptr) {
    return false;
  }
  memcpy(host, rest, host_len);
  host[host_len] = '\0';

  if (colon != nullptr) {
    char* end = nullptr;
    const long value = strtol(colon + 1, &end, 10);
    if (end == colon + 1 || value <= 0 || value > 65535) {
      return false;
    }
    *port = static_cast<uint16_t>(value);
  }
  return true;
}

}  // namespace ssdp
}  // namespace hcp
