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

TEST(Debouncer, SettlingTellsAnEventDrivenCallerToSampleAgain) {
  // The Zephyr backend samples the pin only after an edge interrupt, so it has to
  // know that the first look at a new level is not a conclusion yet. This is the
  // exact sequence button_zephyr.cpp runs: edge -> sample -> re-arm -> sample.
  Debouncer d(30);
  d.Update(false, 0);
  EXPECT_FALSE(d.settling());
  EXPECT_EQ(d.Update(true, 100), Debouncer::kNone);
  EXPECT_TRUE(d.settling());
  EXPECT_EQ(d.Update(true, 130), Debouncer::kPressed);
  EXPECT_FALSE(d.settling());
}

// ---- PressClassifier: short press / long press / no-man's-land ----

using PC = hcp::input::PressClassifier;

TEST(PressClassifier, IdleLowEmitsNothing) {
  PC p(3000, 1000);
  EXPECT_EQ(p.Update(false, 0), PC::kNone);
  EXPECT_EQ(p.Update(false, 500), PC::kNone);
}

TEST(PressClassifier, QuickReleaseIsShort) {
  PC p(3000, 1000);
  EXPECT_EQ(p.Update(true, 100), PC::kNone);   // press edge
  EXPECT_EQ(p.Update(true, 200), PC::kNone);   // still held, below long_ms
  EXPECT_EQ(p.Update(false, 600), PC::kShort); // released within 1 s
}

TEST(PressClassifier, LongPressFiresWhileStillHeldAndOnlyOnce) {
  PC p(3000, 1000);
  EXPECT_EQ(p.Update(true, 0), PC::kNone);
  EXPECT_EQ(p.Update(true, 2950), PC::kNone);
  EXPECT_EQ(p.Update(true, 3000), PC::kLong);  // acts without waiting for release
  EXPECT_EQ(p.Update(true, 3050), PC::kNone);  // no repeat
  EXPECT_EQ(p.Update(false, 5000), PC::kNone);  // release is then meaningless
}

TEST(PressClassifier, ReleaseInBetweenIsIgnored) {
  PC p(3000, 1000);
  EXPECT_EQ(p.Update(true, 0), PC::kNone);
  EXPECT_EQ(p.Update(false, 1500), PC::kAborted);  // neither short nor long
}

TEST(PressClassifier, SecondPressStartsFresh) {
  PC p(3000, 1000);
  EXPECT_EQ(p.Update(true, 0), PC::kNone);
  EXPECT_EQ(p.Update(false, 200), PC::kShort);
  EXPECT_EQ(p.Update(true, 1000), PC::kNone);
  EXPECT_EQ(p.Update(true, 3950), PC::kNone);  // 2.95 s into this press
  EXPECT_EQ(p.Update(true, 4000), PC::kLong);  // 3 s from the new press
}

