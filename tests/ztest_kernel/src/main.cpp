/*
 * ztest for the hcp::kernel Zephyr backend (runs on native_sim).
 * Google C++ Style naming.
 * Copyright (c) 2026 maybetaken
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/ztest.h>
#include <zephyr/sys/atomic.h>

#include "hcp/kernel/kernel.h"

using namespace hcp::kernel;

static MessageQueue<uint32_t> g_q(4);

static void Producer(void* /*arg*/) {
  g_q.Send(7, 100);
  g_q.Send(0xFFu, 100);
}

static atomic_t g_fires;
static void Tick(void* /*arg*/) { atomic_inc(&g_fires); }

ZTEST(hcp_kernel, test_mutex) {
  Mutex m;
  zassert_true(m.TryLock(), "free trylock");
  m.Unlock();
  zassert_true(m.Lock(100), "timed lock");
  m.Unlock();
  {
    ScopedLock s(m);
    zassert_true(s.IsLocked(), "scoped lock acquired");
  }
  zassert_true(m.TryLock(), "free again after scope");
  m.Unlock();
}

ZTEST(hcp_kernel, test_msgqueue_fifo) {
  struct Msg {
    uint32_t a;
  };
  MessageQueue<Msg> q(2);
  Msg x{1}, y{2}, z{3}, got{};
  zassert_true(q.Send(x, 0), "send 1");
  zassert_true(q.Send(y, 0), "send 2");
  zassert_false(q.Send(z, 0), "full non-blocking");
  zassert_equal(q.Count(), 2u, "count");
  zassert_true(q.Receive(got, 0), "recv 1");
  zassert_equal(got.a, 1u, "FIFO 1");
  zassert_true(q.Receive(got, 0), "recv 2");
  zassert_equal(got.a, 2u, "FIFO 2");
  zassert_false(q.Receive(got, 0), "empty non-blocking");
}

ZTEST(hcp_kernel, test_thread_and_queue) {
  uint32_t v;
  while (g_q.Receive(v, 0)) {
  }
  Thread th(Producer, nullptr, "producer");
  zassert_true(th.Start(), "thread start");
  zassert_true(g_q.Receive(v, 1000), "recv from worker");
  zassert_equal(v, 7u, "first value");
  zassert_true(g_q.Receive(v, 1000), "recv sentinel");
  zassert_equal(v, 0xFFu, "second value");
  zassert_true(th.Join(), "thread join");
}

ZTEST(hcp_kernel, test_timer_periodic) {
  atomic_set(&g_fires, 0);
  Timer t(Tick);
  zassert_true(t.StartPeriodic(20), "start periodic");
  time::DelayMs(95);
  t.Stop();
  uint32_t a = static_cast<uint32_t>(atomic_get(&g_fires));
  zassert_true(a >= 2, "timer fired multiple times (got %u)", a);
  time::DelayMs(60);
  zassert_equal(static_cast<uint32_t>(atomic_get(&g_fires)), a, "no fires after stop");
}

ZTEST(hcp_kernel, test_time_monotonic) {
  uint64_t a = time::UptimeMs();
  time::DelayMs(30);
  uint64_t b = time::UptimeMs();
  zassert_true(b - a >= 20, "uptime advanced across delay");
}

ZTEST_SUITE(hcp_kernel, NULL, NULL, NULL, NULL, NULL);
