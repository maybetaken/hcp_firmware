/*
 * hcp::input — Debouncer pure-logic implementation (OS-independent).
 * SPDX-License-Identifier: Apache-2.0
 */
#include "hcp/input/input.h"

namespace hcp {
namespace input {

int Debouncer::Update(bool raw_level, uint64_t now_ms) {
  if (!primed_) {
    primed_ = true;
    stable_ = raw_level;
    pending_ = raw_level;
    last_change_ms_ = now_ms;
    return kNone;
  }

  if (raw_level != pending_) {
    pending_ = raw_level;
    last_change_ms_ = now_ms;
    return kNone;
  }

  if (pending_ != stable_ && (now_ms - last_change_ms_) >= settle_ms_) {
    stable_ = pending_;
    return stable_ ? kPressed : kReleased;
  }

  return kNone;
}

}  // namespace input
}  // namespace hcp
