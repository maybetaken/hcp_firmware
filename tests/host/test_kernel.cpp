/*
 * Copyright (c) 2026 maybetaken
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Host unit tests for the hcp::kernel posix backend (GoogleTest). */
#include <gtest/gtest.h>

#include <atomic>

#include "hcp/kernel/kernel.h"

using namespace hcp::kernel;

static std::atomic<uint32_t> g_thread_count{0};
static void BumpWorker(void* arg) {
  auto* c = static_cast<std::atomic<uint32_t>*>(arg);
  for (int i = 0; i < 100; ++i) {
    c->fetch_add(1);
    time::DelayMs(1);
  }
}

static std::atomic<uint32_t> g_fires{0};
static void TimerTick(void* /*arg*/) { g_fires.fetch_add(1); }

static std::atomic<int> g_seen{0};
static void ReceiverWorker(void* arg) {
  auto* q = static_cast<MessageQueue<int>*>(arg);
  int v = 0;
  if (q->Receive(v, kInfiniteTimeoutMs)) {
    g_seen.store(v);
  }
}

TEST(Mutex, TryLockAndTimeout) {
  Mutex m;
  EXPECT_TRUE(m.TryLock());
  EXPECT_FALSE(m.TryLock());
  EXPECT_FALSE(m.Lock(20));
  m.Unlock();
  EXPECT_TRUE(m.Lock(20));
  m.Unlock();
}

TEST(Mutex, ScopedLockGuards) {
  Mutex m;
  {
    ScopedLock s(m);
    EXPECT_TRUE(s.IsLocked());
    EXPECT_FALSE(m.TryLock());
  }
  EXPECT_TRUE(m.TryLock());
  m.Unlock();
}

struct Msg {
  uint32_t a;
  int32_t b;
};

TEST(MessageQueue, FifoAndNonBlockingBounds) {
  MessageQueue<Msg> q(2);
  Msg x{1, -1}, y{2, -2}, z{3, -3}, got{};
  EXPECT_TRUE(q.Send(x, 0));
  EXPECT_TRUE(q.Send(y, 0));
  EXPECT_FALSE(q.Send(z, 0));
  EXPECT_EQ(q.Count(), 2u);
  EXPECT_TRUE(q.Receive(got, 0));
  EXPECT_EQ(got.a, 1u);
  EXPECT_TRUE(q.Receive(got, 0));
  EXPECT_EQ(got.a, 2u);
  EXPECT_FALSE(q.Receive(got, 0));
  EXPECT_EQ(q.Count(), 0u);
}

TEST(MessageQueue, BlockingCrossThread) {
  MessageQueue<int> q(1);
  g_seen.store(0);
  Thread th(ReceiverWorker, &q);
  ASSERT_TRUE(th.Start());
  EXPECT_TRUE(q.Send(42, 100));
  EXPECT_TRUE(th.Join());
  EXPECT_EQ(g_seen.load(), 42);
}

TEST(Thread, RunsAndJoins) {
  g_thread_count.store(0);
  Thread th(BumpWorker, &g_thread_count);
  ASSERT_TRUE(th.Start());
  EXPECT_TRUE(th.Join());
  EXPECT_EQ(g_thread_count.load(), 100u);
}

TEST(Timer, PeriodicFiresThenStops) {
  g_fires.store(0);
  Timer t(TimerTick);
  ASSERT_TRUE(t.StartPeriodic(20));
  time::DelayMs(95);
  t.Stop();
  uint32_t after = g_fires.load();
  EXPECT_GE(after, 3u);
  time::DelayMs(60);
  EXPECT_EQ(g_fires.load(), after);
}

TEST(Time, UptimeMonotonicAcrossDelay) {
  uint64_t a = time::UptimeMs();
  time::DelayMs(30);
  uint64_t b = time::UptimeMs();
  EXPECT_GE(b - a, 25u);
}
