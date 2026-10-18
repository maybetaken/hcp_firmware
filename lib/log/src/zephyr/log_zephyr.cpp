/*
 * hcp::log — Zephyr backend. Routes to the Zephyr LOG subsystem (single
 * module), gated per-domain by CONFIG_HCP_LOG_LEVEL_*.
 * Google C++ Style naming.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "hcp/log/log.h"

#include <zephyr/logging/log.h>

/* Zephyr uses minimal C++ (-nostdinc++): pull the C variadic/stdio headers. */
#include <stdarg.h>
#include <stdio.h>

LOG_MODULE_REGISTER(hcp_log, LOG_LEVEL_DBG);

#ifndef CONFIG_HCP_LOG_LEVEL_WIFI
#define CONFIG_HCP_LOG_LEVEL_WIFI 3
#endif
#ifndef CONFIG_HCP_LOG_LEVEL_MQTT
#define CONFIG_HCP_LOG_LEVEL_MQTT 3
#endif
#ifndef CONFIG_HCP_LOG_LEVEL_MODBUS
#define CONFIG_HCP_LOG_LEVEL_MODBUS 3
#endif
#ifndef CONFIG_HCP_LOG_LEVEL_DEVICE
#define CONFIG_HCP_LOG_LEVEL_DEVICE 3
#endif
#ifndef CONFIG_HCP_LOG_LEVEL_OTA
#define CONFIG_HCP_LOG_LEVEL_OTA 3
#endif
#ifndef CONFIG_HCP_LOG_LEVEL_SSDP
#define CONFIG_HCP_LOG_LEVEL_SSDP 3
#endif

namespace hcp {
namespace log {

static Level g_levels[kDomainCount] = {
    static_cast<Level>(CONFIG_HCP_LOG_LEVEL_WIFI),
    static_cast<Level>(CONFIG_HCP_LOG_LEVEL_MQTT),
    static_cast<Level>(CONFIG_HCP_LOG_LEVEL_MODBUS),
    static_cast<Level>(CONFIG_HCP_LOG_LEVEL_DEVICE),
    static_cast<Level>(CONFIG_HCP_LOG_LEVEL_OTA),
    static_cast<Level>(CONFIG_HCP_LOG_LEVEL_SSDP),
};

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
  int n = snprintf(buf, sizeof(buf), "[%s] ", DomainName(d));
  if (n < 0) {
    return;
  }
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
  va_end(ap);

  switch (l) {
    case kLevelError: LOG_ERR("%s", buf); break;
    case kLevelWarn:  LOG_WRN("%s", buf); break;
    case kLevelInfo:  LOG_INF("%s", buf); break;
    case kLevelDebug: LOG_DBG("%s", buf); break;
    default: break;
  }
}

}  // namespace log
}  // namespace hcp
