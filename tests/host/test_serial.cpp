/*
 * Copyright (c) 2026 maybetaken
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Host unit tests for the hcp::serial posix loopback backend. */
#include <gtest/gtest.h>

#include "hcp/serial/port.h"

using hcp::serial::Port;

TEST(Serial, LoopbackWriteRead) {
  Port p;
  ASSERT_TRUE(p.Open(115200));
  const uint8_t msg[] = {0x01, 0x03, 0x00, 0x00};
  EXPECT_EQ(p.Write(msg, sizeof(msg)), 4);
  uint8_t rb[8] = {};
  EXPECT_EQ(p.Read(rb, 4, 100), 4);
  EXPECT_EQ(rb[0], 0x01);
  EXPECT_EQ(rb[3], 0x00);
}

TEST(Serial, ReadEmptyTimesOut) {
  Port p;
  ASSERT_TRUE(p.Open(9600));
  uint8_t b[4];
  EXPECT_EQ(p.Read(b, 4, 20), 0);
}

TEST(Serial, WriteWhenClosedFails) {
  Port p;
  const uint8_t x[] = {1};
  EXPECT_EQ(p.Write(x, 1), -1);
}
