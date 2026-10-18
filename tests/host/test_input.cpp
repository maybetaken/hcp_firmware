/* Host unit tests for the hcp::input Debouncer (pure logic). */
#include <gtest/gtest.h>

#include "hcp/input/input.h"

using hcp::input::Debouncer;

TEST(Debouncer, BaselineNoEdge) {
  Debouncer d(30);
  EXPECT_EQ(d.Update(false, 0), Debouncer::kNone);
  EXPECT_FALSE(d.stable());
}

TEST(Debouncer, PressWithBounce) {
  Debouncer d(30);
  d.Update(false, 0);
  EXPECT_EQ(d.Update(true, 10), Debouncer::kNone);
  EXPECT_EQ(d.Update(false, 12), Debouncer::kNone);
  EXPECT_EQ(d.Update(true, 20), Debouncer::kNone);
  EXPECT_EQ(d.Update(true, 45), Debouncer::kNone);
  EXPECT_EQ(d.Update(true, 51), Debouncer::kPressed);
  EXPECT_TRUE(d.stable());
  EXPECT_EQ(d.Update(true, 100), Debouncer::kNone);
}

TEST(Debouncer, ReleaseAfterHold) {
  Debouncer d(30);
  d.Update(true, 0);
  EXPECT_EQ(d.Update(false, 10), Debouncer::kNone);
  EXPECT_EQ(d.Update(false, 25), Debouncer::kNone);
  EXPECT_EQ(d.Update(false, 41), Debouncer::kReleased);
  EXPECT_FALSE(d.stable());
}
