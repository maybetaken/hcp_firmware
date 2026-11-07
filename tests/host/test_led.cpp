/*
 * Copyright (c) 2026 maybetaken
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Host unit tests for the hcp::led posix framebuffer backend. */
#include <gtest/gtest.h>

#include "hcp/led/strip.h"

using hcp::led::Color;
using hcp::led::Strip;

TEST(Led, SetGetPixel) {
  Strip s(4);
  ASSERT_TRUE(s.Open());
  s.SetPixel(1, Color{10, 20, 30});
  Color c{};
  ASSERT_TRUE(s.GetPixel(1, c));
  EXPECT_EQ(c.r, 10);
  EXPECT_EQ(c.g, 20);
  EXPECT_EQ(c.b, 30);
  EXPECT_FALSE(s.GetPixel(4, c));
}

TEST(Led, OutOfRangeSetIgnored) {
  Strip s(2);
  s.Open();
  s.SetPixel(99, Color{1, 1, 1});
  EXPECT_TRUE(s.Show());
}

TEST(Led, SetAllAndClear) {
  Strip s(3);
  s.Open();
  s.SetAll(Color{1, 2, 3});
  Color c{};
  for (uint16_t i = 0; i < 3; ++i) {
    ASSERT_TRUE(s.GetPixel(i, c));
    EXPECT_EQ(c.r, 1);
    EXPECT_EQ(c.g, 2);
    EXPECT_EQ(c.b, 3);
  }
  s.Clear();
  ASSERT_TRUE(s.GetPixel(0, c));
  EXPECT_EQ(c.r, 0);
}

TEST(Led, BrightnessClamps) {
  Strip s(1);
  s.SetBrightness(150);
  EXPECT_EQ(s.Brightness(), 100);
  s.SetBrightness(40);
  EXPECT_EQ(s.Brightness(), 40);
}

/* Frames() counts flushes, not SetAll/Show calls, and says nothing about whether the
 * frame differed. The host has no pixel to be wrong about, so what it can pin is the
 * contract the Zephyr backend is held to as well: one count per flush, starting at
 * zero, and unchanged by a frame that never reached Show(). */
TEST(Led, FramesCountsFlushes) {
  Strip s(2);
  EXPECT_EQ(s.Frames(), 0u);       // opened or not, nothing has gone out yet
  ASSERT_TRUE(s.Open());
  EXPECT_EQ(s.Frames(), 0u);

  s.SetAll(Color{1, 2, 3});
  ASSERT_TRUE(s.Show());
  ASSERT_TRUE(s.Show());           // the host has no no-change check to miss
  EXPECT_EQ(s.Frames(), 2u);

  ASSERT_TRUE(s.Refresh());        // re-asserting an unchanged frame still flushes
  EXPECT_EQ(s.Frames(), 3u);

  /* Nothing can refuse a flush here - there is no driver - so the two fields a failing
   * strip reports on target stay at their initial value rather than disappearing. */
  EXPECT_EQ(s.Errors(), 0u);
  EXPECT_EQ(s.LastError(), 0);
}
