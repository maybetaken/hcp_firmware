/* Host unit tests for the hcp::switchctl posix fake backend. */
#include <gtest/gtest.h>

#include "hcp/switch/relay.h"

using hcp::switchctl::kRelayCount;
using hcp::switchctl::Relay;

TEST(Switch, OpenAndToggle) {
  Relay r;
  ASSERT_TRUE(r.Open(0));
  EXPECT_EQ(r.Index(), 0);
  EXPECT_FALSE(r.Get());
  r.Set(true);
  EXPECT_TRUE(r.Get());
  r.Set(false);
  EXPECT_FALSE(r.Get());
}

TEST(Switch, IndependentInstances) {
  Relay a, b;
  ASSERT_TRUE(a.Open(0));
  ASSERT_TRUE(b.Open(1));
  a.Set(true);
  EXPECT_TRUE(a.Get());
  EXPECT_FALSE(b.Get());
}

TEST(Switch, OpenOutOfRangeFails) {
  Relay r;
  EXPECT_FALSE(r.Open(-1));
  EXPECT_FALSE(r.Open(kRelayCount));
  EXPECT_FALSE(r.Open(99));
}
