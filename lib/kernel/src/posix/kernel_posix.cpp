/*
 * hcp::kernel — POSIX backend (host / Linux, for native unit tests).
 * Google C++ Style naming.
 * Copyright (c) 2026 maybetaken
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE 1

#include "hcp/kernel/kernel.h"

#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <thread>
#include <vector>

namespace hcp {
namespace kernel {

void Reboot() {
  /* Nothing to reset on a host; abort so a test that reaches this line fails
   * loudly instead of returning into code that assumed it would not. */
  std::abort();
}

/* No hardware deadline to set on a host. Returning false is what the caller
 * needs: hcp::ota keeps the behaviour it has on a board without a watchdog. */
bool ArmWatchdog(uint32_t) { return false; }
void DisarmWatchdog() {}

namespace {
void AbsDeadlineMs(struct timespec* ts, uint32_t ms) {
  clock_gettime(CLOCK_REALTIME, ts);
  ts->tv_sec += ms / 1000u;
  ts->tv_nsec += static_cast<long>(ms % 1000u) * 1000000L;
  if (ts->tv_nsec >= 1000000000L) {
    ts->tv_sec += 1;
    ts->tv_nsec -= 1000000000L;
  }
}
}  // namespace

struct Mutex::Impl {
  pthread_mutex_t m;
};

Mutex::Mutex() : impl_(new Impl()) { pthread_mutex_init(&impl_->m, nullptr); }
Mutex::~Mutex() {
  pthread_mutex_destroy(&impl_->m);
  delete impl_;
}
bool Mutex::Lock(uint32_t timeout_ms) {
  if (timeout_ms == kInfiniteTimeoutMs) {
    return pthread_mutex_lock(&impl_->m) == 0;
  }
  struct timespec ts;
  AbsDeadlineMs(&ts, timeout_ms);
  return pthread_mutex_timedlock(&impl_->m, &ts) == 0;
}
bool Mutex::TryLock() { return pthread_mutex_trylock(&impl_->m) == 0; }
void Mutex::Unlock() { pthread_mutex_unlock(&impl_->m); }

namespace time {
uint64_t UptimeMs() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000ull +
         static_cast<uint64_t>(ts.tv_nsec) / 1000000ull;
}
void DelayMs(uint32_t ms) {
  struct timespec req{ms / 1000u, static_cast<long>(ms % 1000u) * 1000000L};
  while (nanosleep(&req, &req) == -1 && errno == EINTR) {
  }
}
}  // namespace time

struct MsgQueueRaw::Impl {
  std::mutex mtx;
  std::condition_variable not_empty;
  std::condition_variable not_full;
  std::vector<uint8_t> buf;
  uint32_t item_size;
  uint32_t max_items;
  uint32_t head = 0;
  uint32_t tail = 0;
  uint32_t count = 0;
};

MsgQueueRaw::MsgQueueRaw(uint32_t item_size, uint32_t max_items)
    : impl_(new Impl()) {
  impl_->item_size = item_size;
  impl_->max_items = max_items ? max_items : 1;
  impl_->buf.resize(static_cast<size_t>(impl_->item_size) * impl_->max_items);
}
MsgQueueRaw::~MsgQueueRaw() { delete impl_; }

bool MsgQueueRaw::Send(const void* item, uint32_t timeout_ms) {
  std::unique_lock<std::mutex> lk(impl_->mtx);
  if (impl_->count == impl_->max_items) {
    if (timeout_ms == 0) {
      return false;
    }
    if (timeout_ms == kInfiniteTimeoutMs) {
      impl_->not_full.wait(lk, [&] { return impl_->count < impl_->max_items; });
    } else if (!impl_->not_full.wait_for(
                   lk, std::chrono::milliseconds(timeout_ms),
                   [&] { return impl_->count < impl_->max_items; })) {
      return false;
    }
  }
  std::memcpy(&impl_->buf[static_cast<size_t>(impl_->tail) * impl_->item_size],
              item, impl_->item_size);
  impl_->tail = (impl_->tail + 1) % impl_->max_items;
  ++impl_->count;
  impl_->not_empty.notify_one();
  return true;
}

bool MsgQueueRaw::Receive(void* item, uint32_t timeout_ms) {
  std::unique_lock<std::mutex> lk(impl_->mtx);
  if (impl_->count == 0) {
    if (timeout_ms == 0) {
      return false;
    }
    if (timeout_ms == kInfiniteTimeoutMs) {
      impl_->not_empty.wait(lk, [&] { return impl_->count > 0; });
    } else if (!impl_->not_empty.wait_for(
                   lk, std::chrono::milliseconds(timeout_ms),
                   [&] { return impl_->count > 0; })) {
      return false;
    }
  }
  std::memcpy(item,
              &impl_->buf[static_cast<size_t>(impl_->head) * impl_->item_size],
              impl_->item_size);
  impl_->head = (impl_->head + 1) % impl_->max_items;
  --impl_->count;
  impl_->not_full.notify_one();
  return true;
}

uint32_t MsgQueueRaw::Count() const {
  std::lock_guard<std::mutex> lk(impl_->mtx);
  return impl_->count;
}

struct Thread::Impl {
  std::thread th;
  Thread::Main fn = nullptr;
  void* arg = nullptr;
  std::atomic<bool> started{false};
};

Thread::Thread(Main fn, void* arg, const char* /*name*/, uint32_t /*prio*/,
               uint32_t /*stack*/)
    : impl_(new Impl()) {
  impl_->fn = fn;
  impl_->arg = arg;
}
Thread::~Thread() {
  if (impl_->th.joinable()) {
    impl_->th.detach();
  }
  delete impl_;
}
bool Thread::Start() {
  bool expected = false;
  if (!impl_->started.compare_exchange_strong(expected, true)) {
    return false;
  }
  Impl* i = impl_;
  impl_->th = std::thread([i] { i->fn(i->arg); });
  return true;
}
bool Thread::Join() {
  if (!impl_->th.joinable()) {
    return false;
  }
  impl_->th.join();
  return true;
}
bool Thread::Joinable() const { return impl_->th.joinable(); }

struct Timer::Impl {
  Timer::Callback cb = nullptr;
  void* arg = nullptr;
  std::mutex mtx;
  std::condition_variable cv;
  std::atomic<bool> running{false};
  std::atomic<bool> stop{false};
  std::thread th;
  uint32_t interval_ms = 0;
};

Timer::Timer(Callback cb, void* arg) : impl_(new Impl()) {
  impl_->cb = cb;
  impl_->arg = arg;
}
Timer::~Timer() {
  Stop();
  delete impl_;
}

bool Timer::StartPeriodic(uint32_t period_ms) {
  Impl* i = impl_;
  if (i->running.exchange(true)) {
    return false;
  }
  i->stop = false;
  i->interval_ms = period_ms;
  i->th = std::thread([i] {
    std::unique_lock<std::mutex> lk(i->mtx);
    while (!i->stop) {
      if (i->cv.wait_for(lk, std::chrono::milliseconds(i->interval_ms),
                         [&] { return i->stop.load(); })) {
        break;
      }
      lk.unlock();
      if (!i->stop && i->cb) {
        i->cb(i->arg);
      }
      lk.lock();
    }
  });
  return true;
}

bool Timer::StartOneShot(uint32_t delay_ms) {
  Impl* i = impl_;
  if (i->running.exchange(true)) {
    return false;
  }
  i->stop = false;
  i->interval_ms = delay_ms;
  i->th = std::thread([i] {
    std::unique_lock<std::mutex> lk(i->mtx);
    if (!i->cv.wait_for(lk, std::chrono::milliseconds(i->interval_ms),
                        [&] { return i->stop.load(); }) &&
        !i->stop && i->cb) {
      lk.unlock();
      i->cb(i->arg);
    }
  });
  return true;
}

void Timer::Stop() {
  Impl* i = impl_;
  {
    std::lock_guard<std::mutex> lk(i->mtx);
    i->stop = true;
  }
  i->cv.notify_all();
  if (i->th.joinable()) {
    i->th.join();
  }
  i->running = false;
}

}  // namespace kernel
}  // namespace hcp
