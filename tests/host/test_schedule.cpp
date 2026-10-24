/*
 * Host tests for hcp::collector's polling logic: when a segment is due, and when a
 * dead slave stops being polled. The clock is injected, so "30 s cooldown" and
 * "staggered periods" are asserted rather than waited for.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <gtest/gtest.h>

#include <string.h>

#include "hcp/collector/schedule.h"
#include "hcp/registry/table.h"

using namespace hcp::collector;
using hcp::registry::Segment;
using hcp::registry::Table;

namespace {

// Two slaves on one bus, one segment each - the multi-subordinate case.
Table TwoSlaves() {
  Table t;
  const char blob[] = "1:0:2:3;15:5120:1:3";
  EXPECT_TRUE(t.Deserialize(blob));
  return t;
}

}  // namespace

TEST(Schedule, EmptyTableHasNothingDue) {
  Schedule s;
  BusHealth health;
  Table t;
  s.Rebuild(t, 0, 2000);
  EXPECT_EQ(s.slots(), 0);
  EXPECT_EQ(s.NextDue(t, health, 100000), -1);
}

TEST(Schedule, StaggeredAndThenPeriodic) {
  Table t = TwoSlaves();
  Schedule s;
  BusHealth health;
  s.Rebuild(t, 0, 2000);
  ASSERT_EQ(s.slots(), 2);
  // not both at t=0: two slaves answering in the same tick would queue behind each
  // other on a half-duplex bus, so the second one starts part-way into the period
  EXPECT_EQ(s.DueAt(1), 1000u);

  EXPECT_EQ(s.NextDue(t, health, 0), 0);
  s.Done(0, 0, 2000);
  EXPECT_EQ(s.NextDue(t, health, 0), -1);       /* second is not due yet */
  EXPECT_EQ(s.NextDue(t, health, 1000), 1);     /* now it is */
  s.Done(1, 1000, 2000);
  EXPECT_EQ(s.NextDue(t, health, 1500), -1);
  EXPECT_EQ(s.NextDue(t, health, 2000), 0);     /* first wrapped */
}

TEST(Schedule, ATableThatChangedWithoutRebuildIsPolledImmediately) {
  Table t = TwoSlaves();
  Schedule s;
  BusHealth health;
  s.Rebuild(t, 0, 2000);

  Table other;
  const char blob[] = "7:100:3:4";
  ASSERT_TRUE(other.Deserialize(blob));

  // slot 0 no longer describes the table: that is a bug in the caller, and the
  // safe answer is "read it now" rather than "silently never poll the new points"
  EXPECT_EQ(s.NextDue(other, health, 0), 0);
}

/* ------------------------------------------------------------------ health -- */

TEST(BusHealth, ThreeFailuresThenBackoff) {
  BusHealth h;
  for (int i = 0; i < BusHealth::kFailLimit - 1; i++) {
    h.NoteResult(15, false, 1000 * i);
    EXPECT_FALSE(h.Skip(15, 1000 * i)) << "still probing before the limit";
  }
  h.NoteResult(15, false, 1000 * BusHealth::kFailLimit);
  EXPECT_TRUE(h.Skip(15, 1000 * BusHealth::kFailLimit));
  EXPECT_TRUE(h.Skip(15, 1000 * BusHealth::kFailLimit + 1));

  // exactly at the cooldown deadline one probe is let through
  const uint64_t probe =
      1000 * BusHealth::kFailLimit + BusHealth::kCooldownMs;
  EXPECT_FALSE(h.Skip(15, probe));

  // and the probe failing pushes the deadline out again
  h.NoteResult(15, false, probe);
  EXPECT_TRUE(h.Skip(15, probe + 1));
  EXPECT_FALSE(h.Skip(15, probe + BusHealth::kCooldownMs));
}

TEST(BusHealth, OneReplyClearsIt) {
  BusHealth h;
  for (int i = 0; i < BusHealth::kFailLimit; i++) {
    h.NoteResult(15, false, 1000 * i);
  }
  ASSERT_TRUE(h.Skip(15, 5000));
  h.NoteResult(15, true, 5000);
  EXPECT_EQ(h.Failures(15), 0);
  EXPECT_FALSE(h.Skip(15, 5000));
}

TEST(BusHealth, OtherUnitsAreNotPunishedForOneDeadSlave) {
  BusHealth h;
  for (int i = 0; i < BusHealth::kFailLimit; i++) {
    h.NoteResult(15, false, 1000 * i);
  }
  ASSERT_TRUE(h.Skip(15, 4000));
  EXPECT_FALSE(h.Skip(1, 4000));
  EXPECT_FALSE(h.Skip(3, 4000));
}

/* --------------------------------------------------------- heartbeat rules -- */

TEST(Heartbeat, ADeviceNobodyMonitorsIsNotRebooted) {
  HeartbeatWatchdog hb;
  EXPECT_FALSE(hb.armed());
  // three hours with no Home Assistant at all is a normal state, not a fault
  EXPECT_FALSE(hb.Expired(3u * 3600u * 1000u));
}

TEST(Heartbeat, FirstBeatArmsTheTimer) {
  HeartbeatWatchdog hb;
  hb.Beat(1000);
  EXPECT_TRUE(hb.armed());
  EXPECT_FALSE(hb.Expired(1000 + HeartbeatWatchdog::kTimeoutMs - 1));
  EXPECT_TRUE(hb.Expired(1000 + HeartbeatWatchdog::kTimeoutMs));
}

TEST(Heartbeat, EachBeatPushesTheDeadlineBack) {
  HeartbeatWatchdog hb;
  hb.Beat(0);
  for (uint64_t t = 5000; t <= 120000; t += 5000) {
    hb.Beat(t);  // Home Assistant publishes every 5 s
    EXPECT_FALSE(hb.Expired(t)) << "now=" << t;
  }
  // and stopping really does time out 60 s later
  EXPECT_FALSE(hb.Expired(120000 + 59999));
  EXPECT_TRUE(hb.Expired(120000 + 60000));
}

TEST(ScheduleAndHealth, BackoffedSlaveDoesNotHoldUpTheBus) {
  Table t = TwoSlaves();
  Schedule s;
  BusHealth h;
  s.Rebuild(t, 0, 2000);

  // unit 15 (slot 1) is dead and unit 1 answers: three probes of 15 fail
  for (int i = 0; i < BusHealth::kFailLimit; i++) {
    const uint64_t now = (uint64_t)i * 2000;
    s.Done(1, now, 2000);
    h.NoteResult(15, false, now);
    s.Done(0, now, 2000);
    h.NoteResult(1, true, now);
  }
  ASSERT_TRUE(h.Skip(15, 8000));
  ASSERT_FALSE(h.Skip(1, 8000));

  // with 15 in backoff, the living slave keeps its own period all the way through
  // the cooldown - one absent meter must not delay everybody else
  for (uint64_t now = 6000; now < 30000; now += 2000) {
    ASSERT_EQ(s.NextDue(t, h, now), 0) << "now=" << now;
    s.Done(0, now, 2000);
  }
}
