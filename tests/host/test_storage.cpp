/*
 * Copyright (c) 2026 maybetaken
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Host unit tests for the hcp::storage posix backend. */
#include <gtest/gtest.h>

#include "hcp/storage/store.h"

using namespace hcp::storage;

static const char* kDir = "./hcp_store_test";

TEST(Storage, StringRoundtrip) {
  Store s;
  ASSERT_TRUE(s.Open(kDir));
  ASSERT_TRUE(s.SetString("sn", "ABC123"));
  char buf[32] = {};
  ASSERT_TRUE(s.GetString("sn", buf, sizeof(buf)));
  EXPECT_STREQ(buf, "ABC123");
  EXPECT_TRUE(s.Exists("sn"));
  EXPECT_TRUE(s.Erase("sn"));
  EXPECT_FALSE(s.Exists("sn"));
  char o[8];
  EXPECT_FALSE(s.GetString("missing", o, sizeof(o)));
}

TEST(Storage, IntRoundtrip) {
  Store s;
  ASSERT_TRUE(s.Open(kDir));
  ASSERT_TRUE(s.SetInt("baud", 115200));
  int32_t v = 0;
  ASSERT_TRUE(s.GetInt("baud", v));
  EXPECT_EQ(v, 115200);
  s.SetInt("baud", 9600);
  s.GetInt("baud", v);
  EXPECT_EQ(v, 9600);
  s.Erase("baud");
}

TEST(Storage, GetIntMissingFails) {
  Store s;
  ASSERT_TRUE(s.Open(kDir));
  s.Erase("nope");
  int32_t v = 42;
  EXPECT_FALSE(s.GetInt("nope", v));
  EXPECT_EQ(v, 42);
}
