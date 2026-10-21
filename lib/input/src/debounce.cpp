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

/*
 * hcp::input - PressClassifier (also OS-independent): turn held-level samples with
 * timestamps into short/long press actions. See input.h for the contract.
 */
namespace hcp {
namespace input {

PressClassifier::Action PressClassifier::Update(bool pressed, uint64_t now_ms) {
  if (pressed && !held_) {  // press edge
    held_ = true;
    long_fired_ = false;
    start_ms_ = now_ms;
    return kNone;
  }

  if (!pressed) {
    if (!held_) {
      return kNone;  // idle low
    }
    held_ = false;
    if (long_fired_) {
      return kNone;  // the long press already acted; the release means nothing
    }
    uint64_t duration = now_ms - start_ms_;
    return duration <= short_max_ms_ ? kShort : kAborted;
  }

  // Still held: a long press must act immediately, not on release.
  if (!long_fired_ && (now_ms - start_ms_) >= long_ms_) {
    long_fired_ = true;
    return kLong;
  }
  return kNone;
}

}  // namespace input
}  // namespace hcp
