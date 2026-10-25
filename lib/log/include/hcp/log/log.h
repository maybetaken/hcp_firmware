/*
 * HCP logging abstraction — domain-tagged, level-filtered logging.
 * Replaces the legacy FreeRTOS `logging.h`. Backend selected at build time:
 * Zephyr `LOG` on-target, fprintf on host. Google C++ Style naming.
 *
 * Copyright (c) 2026 maybetaken
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

namespace hcp {
namespace log {

enum Domain {
  kDomainWifi = 0,
  kDomainMqtt,
  kDomainModbus,
  kDomainDevice,
  kDomainOta,
  kDomainSsdp,
  kDomainCount,
};

enum Level {
  kLevelOff = 0,
  kLevelError,
  kLevelWarn,
  kLevelInfo,
  kLevelDebug,
};

void SetLevel(Domain d, Level l);
Level GetLevel(Domain d);

const char* DomainName(Domain d);
const char* LevelName(Level l);

// Core emit function (level-gated per domain internally).
void Write(Domain d, Level l, const char* file, int line, const char* fmt, ...)
    __attribute__((format(printf, 5, 6)));

}  // namespace log
}  // namespace hcp

#define HCP_LOG(dom, lvl, ...) \
  ::hcp::log::Write((dom), (lvl), __FILE__, __LINE__, __VA_ARGS__)

#define HCP_LOGE(dom, ...) HCP_LOG(dom, ::hcp::log::kLevelError, __VA_ARGS__)
#define HCP_LOGW(dom, ...) HCP_LOG(dom, ::hcp::log::kLevelWarn, __VA_ARGS__)
#define HCP_LOGI(dom, ...) HCP_LOG(dom, ::hcp::log::kLevelInfo, __VA_ARGS__)
#define HCP_LOGD(dom, ...) HCP_LOG(dom, ::hcp::log::kLevelDebug, __VA_ARGS__)
