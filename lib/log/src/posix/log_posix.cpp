/*
 * hcp::log — POSIX backend (host). Prints to stderr with domain/level tags.
 * Google C++ Style naming.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "hcp/log/log.h"

#include <cstdarg>
#include <cstdio>

namespace hcp {
namespace log {

static Level g_levels[kDomainCount] = {
    kLevelInfo, kLevelInfo, kLevelInfo, kLevelInfo, kLevelInfo, kLevelInfo};

static const char* const kDomainNames[kDomainCount] = {
    "wifi", "mqtt", "modbus", "device", "ota", "ssdp"};
static const char* const kLevelNames[] = {"off", "err", "wrn", "inf", "dbg"};

void SetLevel(Domain d, Level l) {
  if (d < kDomainCount) {
    g_levels[d] = l;
  }
}

Level GetLevel(Domain d) { return (d < kDomainCount) ? g_levels[d] : kLevelOff; }

const char* DomainName(Domain d) {
  return (d < kDomainCount) ? kDomainNames[d] : "?";
}

const char* LevelName(Level l) {
  return (l <= kLevelDebug) ? kLevelNames[l] : "?";
}

void Write(Domain d, Level l, const char* /*file*/, int /*line*/,
           const char* fmt, ...) {
  if (l == kLevelOff || l > GetLevel(d)) {
    return;
  }
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  fprintf(stderr, "[%s %s] %s\n", DomainName(d), LevelName(l), buf);
}

}  // namespace log
}  // namespace hcp
