/*
 * Copyright (c) 2026 maybetaken
 *
 * SPDX-License-Identifier: Apache-2.0
 */

// Host tests for hcp::mqtt's pure pieces (the reconnect backoff schedule).
#include "hcp/mqtt/backoff.h"

#include <gtest/gtest.h>

using hcp::mqtt::Backoff;

TEST(Backoff, FirstRetryIsImmediateThenDoubles) {
  Backoff b(1000, 30000);
  EXPECT_EQ(b.NextMs(), 0u);      // a glitch should be retried right away
  EXPECT_EQ(b.NextMs(), 1000u);
  EXPECT_EQ(b.NextMs(), 2000u);
  EXPECT_EQ(b.NextMs(), 4000u);
}

TEST(Backoff, CapsAtTheCeiling) {
  Backoff b(1000, 8000);
  uint32_t last = 0;
  for (int i = 0; i < 12; i++) {
    last = b.NextMs();
    EXPECT_LE(last, 8000u);
  }
  EXPECT_EQ(last, 8000u);  // it settles at the cap instead of growing
}

TEST(Backoff, ResetRestartsTheSequence) {
  Backoff b(1000, 30000);
  b.NextMs();
  b.NextMs();
  b.Reset();  // a session was established
  EXPECT_EQ(b.NextMs(), 0u);
  EXPECT_EQ(b.NextMs(), 1000u);
}

TEST(Backoff, BaseEqualCapStillTerminates) {
  Backoff b(5000, 5000);
  EXPECT_EQ(b.NextMs(), 0u);
  EXPECT_EQ(b.NextMs(), 5000u);
  EXPECT_EQ(b.NextMs(), 5000u);
}
