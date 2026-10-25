/*
 * hcp::serial — POSIX backend (host). In-memory loopback: bytes written come
 * back on read. Google C++ Style naming.
 * Copyright (c) 2026 maybetaken
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "hcp/serial/port.h"

#include <chrono>
#include <deque>
#include <thread>

namespace hcp {
namespace serial {

struct Port::Impl {
  std::deque<uint8_t> buf;
  bool open = false;
};

Port::Port() : impl_(new Impl()) {}
Port::~Port() { delete impl_; }

bool Port::Open(uint32_t /*baud*/) {
  impl_->open = true;
  return true;
}
void Port::Close() { impl_->open = false; }
bool Port::IsOpen() const { return impl_->open; }

int Port::Write(const uint8_t* data, size_t len) {
  if (!impl_->open) {
    return -1;
  }
  impl_->buf.insert(impl_->buf.end(), data, data + len);
  return static_cast<int>(len);
}

int Port::Read(uint8_t* buf, size_t cap, uint32_t timeout_ms) {
  if (!impl_->open || cap == 0) {
    return -1;
  }
  auto start = std::chrono::steady_clock::now();
  size_t got = 0;
  while (got < cap) {
    if (!impl_->buf.empty()) {
      buf[got++] = impl_->buf.front();
      impl_->buf.pop_front();
      continue;
    }
    if (got > 0) {
      break;
    }
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - start)
            .count() >= static_cast<int64_t>(timeout_ms)) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return static_cast<int>(got);
}

}  // namespace serial
}  // namespace hcp
