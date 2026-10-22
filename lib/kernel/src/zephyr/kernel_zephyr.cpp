/*
 * hcp::kernel — Zephyr backend (on-target).
 * Mutex->k_mutex, MsgQueue->k_msgq, Thread->static k_thread pool,
 * Timer->k_timer, time->k_uptime_get/k_sleep.
 * Google C++ Style naming.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "hcp/kernel/kernel.h"

#include <zephyr/kernel.h>
#include <zephyr/irq.h>
#include <zephyr/sys/atomic.h>

#include <cstdint>
#include <new>

namespace hcp {
namespace kernel {

namespace time {
uint64_t UptimeMs() { return static_cast<uint64_t>(k_uptime_get()); }
void DelayMs(uint32_t ms) { k_sleep(K_MSEC(ms)); }
}  // namespace time

/* ESP32 ROM entry point, resolved from the ROM symbol table at link time: the
 * only reset path that exists in a Zephyr build of these SoCs (see the header). */
extern "C" void esp_rom_software_reset_system(void);

void Reboot() {
  /* Interrupts off so nothing can touch memory while the cache and the SoC are
   * being torn down. */
  (void)irq_lock();
  esp_rom_software_reset_system();
  while (true) {
  }
}

struct Mutex::Impl {
  struct k_mutex m;
};
Mutex::Mutex() : impl_(new Impl()) { k_mutex_init(&impl_->m); }
Mutex::~Mutex() { delete impl_; }
bool Mutex::Lock(uint32_t timeout_ms) {
  k_timeout_t to = (timeout_ms == kInfiniteTimeoutMs) ? K_FOREVER : K_MSEC(timeout_ms);
  return k_mutex_lock(&impl_->m, to) == 0;
}
bool Mutex::TryLock() { return k_mutex_lock(&impl_->m, K_NO_WAIT) == 0; }
void Mutex::Unlock() { k_mutex_unlock(&impl_->m); }

struct MsgQueueRaw::Impl {
  struct k_msgq q;
  uint8_t* buf;
};
MsgQueueRaw::MsgQueueRaw(uint32_t item_size, uint32_t max_items)
    : impl_(new Impl()) {
  uint32_t cap = max_items ? max_items : 1;
  impl_->buf = new uint8_t[item_size * cap];
  k_msgq_init(&impl_->q, reinterpret_cast<char*>(impl_->buf), item_size, cap);
}
MsgQueueRaw::~MsgQueueRaw() {
  k_msgq_cleanup(&impl_->q);
  delete[] impl_->buf;
  delete impl_;
}
static k_timeout_t ToOf(uint32_t ms) {
  return (ms == kInfiniteTimeoutMs) ? K_FOREVER : K_MSEC(ms);
}
bool MsgQueueRaw::Send(const void* item, uint32_t timeout_ms) {
  return k_msgq_put(&impl_->q, item, ToOf(timeout_ms)) == 0;
}
bool MsgQueueRaw::Receive(void* item, uint32_t timeout_ms) {
  return k_msgq_get(&impl_->q, item, ToOf(timeout_ms)) == 0;
}
uint32_t MsgQueueRaw::Count() const { return k_msgq_num_used_get(&impl_->q); }

#ifndef CONFIG_HCP_KERNEL_MAX_THREADS
#define CONFIG_HCP_KERNEL_MAX_THREADS 8
#endif
#ifndef CONFIG_HCP_KERNEL_THREAD_STACK_SIZE
#define CONFIG_HCP_KERNEL_THREAD_STACK_SIZE 2048
#endif
#define HCP_MAX_THREADS CONFIG_HCP_KERNEL_MAX_THREADS

namespace {
struct Slot {
  bool used = false;
  struct k_sem done;
  Thread::Main fn = nullptr;
  void* arg = nullptr;
};
k_thread g_threads[HCP_MAX_THREADS];
K_THREAD_STACK_ARRAY_DEFINE(g_stacks, HCP_MAX_THREADS,
                            CONFIG_HCP_KERNEL_THREAD_STACK_SIZE);
Slot g_slots[HCP_MAX_THREADS];
K_MUTEX_DEFINE(g_pool_lock);

void Entry(void* p1, void* /*p2*/, void* /*p3*/) {
  int s = static_cast<int>(reinterpret_cast<intptr_t>(p1));
  Thread::Main fn = g_slots[s].fn;
  void* arg = g_slots[s].arg;
  if (fn) {
    fn(arg);
  }
  k_sem_give(&g_slots[s].done);
}
}  // namespace

struct Thread::Impl {
  Thread::Main fn = nullptr;
  void* arg = nullptr;
  uint32_t prio = 0;
  int slot = -1;
  bool started = false;
};

Thread::Thread(Main fn, void* arg, const char* /*name*/, uint32_t priority,
               uint32_t /*stack*/)
    : impl_(new Impl()) {
  impl_->fn = fn;
  impl_->arg = arg;
  impl_->prio = priority;
}
Thread::~Thread() {
  if (impl_->started && impl_->slot >= 0) {
    int s = impl_->slot;
    k_mutex_lock(&g_pool_lock, K_FOREVER);
    g_slots[s].used = false;
    k_mutex_unlock(&g_pool_lock);
  }
  delete impl_;
}
bool Thread::Start() {
  if (impl_->started) {
    return false;
  }
  int chosen = -1;
  k_mutex_lock(&g_pool_lock, K_FOREVER);
  for (int i = 0; i < HCP_MAX_THREADS; ++i) {
    if (!g_slots[i].used) {
      g_slots[i].used = true;
      chosen = i;
      break;
    }
  }
  k_mutex_unlock(&g_pool_lock);
  if (chosen < 0) {
    return false;
  }
  k_sem_init(&g_slots[chosen].done, 0, 1);
  g_slots[chosen].fn = impl_->fn;
  g_slots[chosen].arg = impl_->arg;
  impl_->slot = chosen;
  int prio = static_cast<int>(impl_->prio);
  if (prio < 0) prio = 0;
  if (prio > 14) prio = 14;
  k_thread_create(&g_threads[chosen], g_stacks[chosen],
                  K_THREAD_STACK_SIZEOF(g_stacks[chosen]), Entry,
                  reinterpret_cast<void*>(static_cast<intptr_t>(chosen)),
                  nullptr, nullptr, prio, 0, K_NO_WAIT);
  impl_->started = true;
  return true;
}
bool Thread::Join() {
  if (!impl_->started || impl_->slot < 0) {
    return false;
  }
  int s = impl_->slot;
  k_sem_take(&g_slots[s].done, K_FOREVER);
  k_mutex_lock(&g_pool_lock, K_FOREVER);
  g_slots[s].used = false;
  k_mutex_unlock(&g_pool_lock);
  impl_->started = false;
  impl_->slot = -1;
  return true;
}
bool Thread::Joinable() const { return impl_->started; }

namespace {
struct TimerPub {
  Timer::Callback cb = nullptr;
  void* arg = nullptr;
};
void TimerFired(struct k_timer* t) {
  auto* p = static_cast<TimerPub*>(k_timer_user_data_get(t));
  if (p && p->cb) {
    p->cb(p->arg);
  }
}
}  // namespace

struct Timer::Impl {
  struct k_timer t;
  TimerPub pub;
  bool running = false;
};
Timer::Timer(Callback cb, void* arg) : impl_(new Impl()) {
  impl_->pub.cb = cb;
  impl_->pub.arg = arg;
  k_timer_init(&impl_->t, TimerFired, nullptr);
  k_timer_user_data_set(&impl_->t, &impl_->pub);
}
Timer::~Timer() {
  Stop();
  delete impl_;
}
bool Timer::StartPeriodic(uint32_t period_ms) {
  k_timer_start(&impl_->t, K_MSEC(period_ms), K_MSEC(period_ms));
  impl_->running = true;
  return true;
}
bool Timer::StartOneShot(uint32_t delay_ms) {
  k_timer_start(&impl_->t, K_MSEC(delay_ms), K_NO_WAIT);
  impl_->running = true;
  return true;
}
void Timer::Stop() {
  k_timer_stop(&impl_->t);
  impl_->running = false;
}

}  // namespace kernel
}  // namespace hcp
