/*
 * HCP kernel abstraction — a thin, backend-agnostic wrapper over the OS
 * primitives (thread / mutex / message queue / timer / tick / delay).
 *
 * Naming follows the Google C++ Style Guide:
 *   types PascalCase, methods/functions PascalCase, params snake_case,
 *   class members trailing underscore, constants/enumerators kCamelCase.
 * Backends selected at build time: posix (host unit tests) or zephyr (target).
 * app/core only includes THIS header and never touches pthread/k_thread directly.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <cstdint>

namespace hcp {
namespace kernel {

// Value meaning "wait forever" for every timeout-taking API below.
constexpr uint32_t kInfiniteTimeoutMs = 0xFFFFFFFFu;

// Mutex (RAII-friendly).
class Mutex {
 public:
  Mutex();
  ~Mutex();
  Mutex(const Mutex&) = delete;
  Mutex& operator=(const Mutex&) = delete;

  bool Lock(uint32_t timeout_ms = kInfiniteTimeoutMs);
  bool TryLock();
  void Unlock();

 private:
  struct Impl;
  Impl* impl_;
};

// Scoped lock — locks on construction, unlocks on destruction.
class ScopedLock {
 public:
  explicit ScopedLock(Mutex& mutex) : mutex_(mutex), locked_(mutex.Lock()) {}
  ~ScopedLock() {
    if (locked_) {
      mutex_.Unlock();
    }
  }
  ScopedLock(const ScopedLock&) = delete;
  ScopedLock& operator=(const ScopedLock&) = delete;
  bool IsLocked() const { return locked_; }

 private:
  Mutex& mutex_;
  bool locked_;
};

// Time / tick / delay.
namespace time {
uint64_t UptimeMs();
void DelayMs(uint32_t ms);
}  // namespace time

// Thread.
class Thread {
 public:
  using Main = void (*)(void* arg);

  Thread(Main fn, void* arg, const char* name = "", uint32_t priority = 0,
         uint32_t stack_size = 4096);
  ~Thread();
  Thread(const Thread&) = delete;
  Thread& operator=(const Thread&) = delete;

  bool Start();
  bool Join();
  bool Joinable() const;

 private:
  struct Impl;
  Impl* impl_;
};

// Raw byte message queue (fixed item size, bounded). MessageQueue<T> wraps it.
class MsgQueueRaw {
 public:
  MsgQueueRaw(uint32_t item_size, uint32_t max_items);
  ~MsgQueueRaw();
  MsgQueueRaw(const MsgQueueRaw&) = delete;
  MsgQueueRaw& operator=(const MsgQueueRaw&) = delete;

  bool Send(const void* item, uint32_t timeout_ms = 0);
  bool Receive(void* item, uint32_t timeout_ms = kInfiniteTimeoutMs);
  uint32_t Count() const;
  bool Empty() const { return Count() == 0; }

 private:
  struct Impl;
  Impl* impl_;
};

// Type-safe, bounded message queue.
template <typename T>
class MessageQueue {
 public:
  explicit MessageQueue(uint32_t max_items) : queue_(sizeof(T), max_items) {}

  bool Send(const T& item, uint32_t timeout_ms = 0) {
    return queue_.Send(&item, timeout_ms);
  }
  bool Receive(T& item, uint32_t timeout_ms = kInfiniteTimeoutMs) {
    return queue_.Receive(&item, timeout_ms);
  }
  uint32_t Count() const { return queue_.Count(); }

 private:
  MsgQueueRaw queue_;
};

// Timer — one-shot / periodic callback invoked on a backend timer context.
class Timer {
 public:
  using Callback = void (*)(void* arg);

  Timer(Callback cb, void* arg = nullptr);
  ~Timer();
  Timer(const Timer&) = delete;
  Timer& operator=(const Timer&) = delete;

  bool StartPeriodic(uint32_t period_ms);
  bool StartOneShot(uint32_t delay_ms);
  void Stop();

 private:
  struct Impl;
  Impl* impl_;
};

}  // namespace kernel
}  // namespace hcp
