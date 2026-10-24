/*
 * hcp::collector - see schedule.h for what these two classes are for.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "hcp/collector/schedule.h"

#include <string.h>

namespace hcp {
namespace collector {

/* ----------------------------------------------------------------- Schedule -- */

void Schedule::Rebuild(const registry::Table& table, uint64_t now_ms,
                       uint32_t period_ms) {
  count_ = table.count() > registry::kMaxSegments ? registry::kMaxSegments
                                                   : table.count();
  for (int i = 0; i < count_; i++) {
    slots_[i].unit = table.at(i).slave_id;
    slots_[i].start = table.at(i).start_address;
    // staggered, not all at once: N slaves answering within the same tick would
    // queue up behind each other on a half-duplex bus for no reason
    slots_[i].next_due_ms = now_ms + (uint64_t)period_ms * (uint64_t)i / (uint64_t)(count_ > 0 ? count_ : 1);
  }
}

int Schedule::NextDue(const registry::Table& table, const BusHealth& health,
                      uint64_t now_ms) const {
  int best = -1;
  uint64_t best_due = 0;
  const int n = table.count() < count_ ? table.count() : count_;

  for (int i = 0; i < n; i++) {
    // a segment that no longer matches the table means the caller forgot to
    // rebuild after a config change; treat it as due rather than skip it quietly
    const bool matches = slots_[i].unit == table.at(i).slave_id &&
                         slots_[i].start == table.at(i).start_address;
    if (!matches) {
      return i;
    }
    if (health.Skip(table.at(i).slave_id, now_ms)) {
      continue;
    }
    if (slots_[i].next_due_ms <= now_ms && (best < 0 || slots_[i].next_due_ms < best_due)) {
      best = i;
      best_due = slots_[i].next_due_ms;
    }
  }
  return best;
}

void Schedule::Done(int index, uint64_t now_ms, uint32_t period_ms) {
  if (index >= 0 && index < count_) {
    slots_[index].next_due_ms = now_ms + period_ms;
  }
}

/* ---------------------------------------------------------------- BusHealth -- */

BusHealth::Entry* BusHealth::SlotFor(uint8_t unit, bool create) {
  Entry* free_slot = nullptr;
  for (auto& e : entries_) {
    if (e.used && e.unit == unit) {
      return &e;
    }
    if (!e.used && free_slot == nullptr) {
      free_slot = &e;
    }
  }
  if (!create || free_slot == nullptr) {
    return nullptr;  // full: the oldest-tracked set is fixed size on purpose
  }
  free_slot->used = true;
  free_slot->unit = unit;
  free_slot->failures = 0;
  free_slot->retry_after_ms = 0;
  return free_slot;
}

void BusHealth::NoteResult(uint8_t unit, bool ok, uint64_t now_ms) {
  Entry* e = SlotFor(unit, !ok);
  if (e == nullptr) {
    return;  // healthy and not tracked: nothing to clear
  }
  if (ok) {
    e->failures = 0;
    e->retry_after_ms = 0;
    return;
  }
  if (e->failures < kFailLimit) {
    e->failures++;
    if (e->failures == kFailLimit) {
      e->retry_after_ms = now_ms + kCooldownMs;
    }
  } else {
    // a probe failed: wait another cooldown before the next one
    e->retry_after_ms = now_ms + kCooldownMs;
  }
}

bool BusHealth::Skip(uint8_t unit, uint64_t now_ms) const {
  for (const auto& e : entries_) {
    if (!e.used || e.unit != unit) {
      continue;
    }
    if (e.failures < kFailLimit) {
      return false;
    }
    // at/after the deadline let exactly one attempt through (the probe). The
    // caller's NoteResult either clears the counter or pushes the deadline out.
    return now_ms < e.retry_after_ms;
  }
  return false;
}

int BusHealth::Failures(uint8_t unit) const {
  for (const auto& e : entries_) {
    if (e.used && e.unit == unit) {
      return e.failures;
    }
  }
  return 0;
}

void BusHealth::Clear() {
  // `used` is the only field that must be reset for correctness, but zeroing the
  // whole array also resets failures/retry_after so a stale entry can never come
  // back to life if the same unit address reappears in a new table.
  memset(entries_, 0, sizeof(entries_));
}

}  // namespace collector
}  // namespace hcp
