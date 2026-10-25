/*
 * Copyright (c) 2026 maybetaken
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Host unit tests for the hcp::log posix backend. */
#include <gtest/gtest.h>

#include "hcp/log/log.h"

using namespace hcp::log;

TEST(Log, LevelGetSet) {
  SetLevel(kDomainOta, kLevelWarn);
  EXPECT_EQ(GetLevel(kDomainOta), kLevelWarn);
  SetLevel(kDomainOta, kLevelDebug);
  EXPECT_EQ(GetLevel(kDomainOta), kLevelDebug);
}

TEST(Log, OutOfRangeGuards) {
  SetLevel(static_cast<Domain>(kDomainCount), kLevelInfo);
  EXPECT_EQ(GetLevel(static_cast<Domain>(kDomainCount)), kLevelOff);
}

TEST(Log, Names) {
  EXPECT_STREQ(DomainName(kDomainWifi), "wifi");
  EXPECT_STREQ(DomainName(kDomainModbus), "modbus");
  EXPECT_STREQ(LevelName(kLevelError), "err");
  EXPECT_STREQ(LevelName(kLevelDebug), "dbg");
}

TEST(Log, WriteRespectsGating) {
  SetLevel(kDomainMqtt, kLevelError);
  HCP_LOGI(kDomainMqtt, "suppressed %d", 1);
  HCP_LOGW(kDomainMqtt, "also suppressed");
  HCP_LOGE(kDomainMqtt, "emitted %s", "ok");
  SUCCEED();
}
