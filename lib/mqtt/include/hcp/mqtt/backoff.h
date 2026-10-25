/*
 * hcp::mqtt - reconnection backoff (pure logic, host-tested).
 *
 * A broker that is down must not be hammered, and the first retry after a
 * failure should still feel immediate. So: no wait for the first attempt, then
 * base_ms doubling up to cap_ms, reset once a session is established.
 *
 * Copyright (c) 2026 maybetaken
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <cstdint>

namespace hcp {
namespace mqtt {

class Backoff {
 public:
  Backoff(uint32_t base_ms, uint32_t cap_ms) : base_ms_(base_ms), cap_ms_(cap_ms) {}

  // Delay to wait before the next connection attempt; 0 means "go now".
  uint32_t NextMs();

  // A session came up: start over.
  void Reset() { step_ms_ = 0; }

 private:
  uint32_t base_ms_;
  uint32_t cap_ms_;
  uint32_t step_ms_ = 0;
};

}  // namespace mqtt
}  // namespace hcp
