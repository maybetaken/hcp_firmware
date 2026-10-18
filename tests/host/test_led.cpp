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
