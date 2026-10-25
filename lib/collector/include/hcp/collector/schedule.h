/*
 * hcp::collector - what to poll and when, without touching the bus.
 *
 * Two pieces of pure logic, both driven by an injected clock so they are tested rather
 * than judged on the bench:
 *
 *   Schedule   per-segment due times instead of the previous firmware's one-segment-per-
 *              second round robin. That bounded bus time per cycle - its real purpose -
 *              but a 5-segment table only refreshed every 5 s however cheap the reads
 *              were; here a segment is still never more than one transaction deep.
 *
 *   BusHealth  why a dead slave must not be polled every cycle: RS-485 is half-duplex and
 *              the master waits, so one absent meter costs its full response timeout per
 *              round - with five segments behind it, everything else is minutes late.
 *              After a few consecutive failures a unit cools off and is probed once; a
 *              reply brings it straight back.
 *
 * Copyright (c) 2026 maybetaken
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>

#include "hcp/registry/table.h"

namespace hcp {
namespace collector {

class BusHealth;  // defined below; Schedule consults it before offering a segment

// Tracks when each segment of the table is next worth reading. Positional: build it
// from a table and hand it that same table back, and rebuild when the table changes.
class Schedule {
 public:
  void Rebuild(const registry::Table& table, uint64_t now_ms, uint32_t period_ms);

  // Index of the next segment that is due and whose unit is not in backoff, or -1.
  int NextDue(const registry::Table& table, const BusHealth& health,
              uint64_t now_ms) const;

  // A transaction for slot `index` finished: it is next due after `period_ms`.
  void Done(int index, uint64_t now_ms, uint32_t period_ms);

  int slots() const { return count_; }
  uint64_t DueAt(int index) const { return slots_[index].next_due_ms; }

 private:
  struct Slot {
    uint8_t unit = 0;
    uint16_t start = 0;
    uint64_t next_due_ms = 0;
  };

  Slot slots_[registry::kMaxSegments];
  int count_ = 0;
};

// Per-unit failure tracking. Only the unit matters (one address, one device), not
// which of its segments failed.
class BusHealth {
 public:
  static constexpr int kTrackedUnits = 8;
  static constexpr int kFailLimit = 3;
  static constexpr uint64_t kCooldownMs = 30000;

  void NoteResult(uint8_t unit, bool ok, uint64_t now_ms);

  // True while the unit is in backoff and this is not the moment to probe it.
  bool Skip(uint8_t unit, uint64_t now_ms) const;

  int Failures(uint8_t unit) const;

  // Forget every failure count. Called when the point table changes: a unit that
  // did not answer yesterday may be the meter somebody just wired up, and a table
  // that no longer contains a unit must not keep its stale backoff either.
  void Clear();

 private:
  struct Entry {
    uint8_t unit = 0;
    int failures = 0;
    uint64_t retry_after_ms = 0;
    bool used = false;
  };

  Entry* SlotFor(uint8_t unit, bool create);
  Entry entries_[kTrackedUnits];
};

/*
 * Home Assistant's supervision heartbeat. While the integration is loaded it
 * publishes <sn>/host/heartbeat every 5 s, and the previous firmware rebooted the
 * device when that stopped for 60 s.
 *
 * The rule has one subtlety that must not be lost in a rewrite: the timer is ARMED
 * BY THE FIRST HEARTBEAT, not by boot. A device with no Home Assistant at all -
 * freshly flashed, on the bench, provisioned from a phone - would otherwise reboot
 * once a minute forever, which reads as a broken device rather than an unmonitored
 * one.
 */
class HeartbeatWatchdog {
 public:
  static constexpr uint64_t kTimeoutMs = 60000;

  void Beat(uint64_t now_ms) {
    armed_ = true;
    last_beat_ms_ = now_ms;
  }

  // True when supervision was armed and the host has gone quiet.
  bool Expired(uint64_t now_ms) const {
    return armed_ && (now_ms - last_beat_ms_) >= kTimeoutMs;
  }

  bool armed() const { return armed_; }
  uint64_t SinceBeat(uint64_t now_ms) const { return now_ms - last_beat_ms_; }

 private:
  bool armed_ = false;
  uint64_t last_beat_ms_ = 0;
};

}  // namespace collector
}  // namespace hcp
