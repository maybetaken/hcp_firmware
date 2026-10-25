/*
 * hcp::mqtt - Backoff implementation (OS-independent).
 * Copyright (c) 2026 maybetaken
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "hcp/mqtt/backoff.h"

namespace hcp {
namespace mqtt {

uint32_t Backoff::NextMs() {
  if (step_ms_ == 0) {
    // First retry goes out straight away; only subsequent ones wait.
    step_ms_ = base_ms_;
    return 0;
  }
  uint32_t wait = step_ms_;
  // Double for next time, saturating at the cap (and guarding overflow).
  step_ms_ = (wait >= cap_ms_ / 2) ? cap_ms_ : wait * 2u;
  return wait > cap_ms_ ? cap_ms_ : wait;
}

}  // namespace mqtt
}  // namespace hcp
