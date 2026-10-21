/*
 * HCP input abstraction — button reading with software debounce.
 * Debouncer is pure logic (host-testable); Button binds the devicetree
 * `sw0` gpio-keys alias on the Zephyr backend. Google C++ Style naming.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <cstdint>

namespace hcp {
namespace input {

// Pure software debouncer. Feed raw (logically-oriented) level samples with a
// monotonic timestamp; it emits edges once the input settles.
class Debouncer {
 public:
  enum Event {
    kNone = 0,
    kPressed = 1,
    kReleased = 2,
  };

  explicit Debouncer(uint32_t settle_ms) : settle_ms_(settle_ms) {}

  int Update(bool raw_level, uint64_t now_ms);
  bool stable() const { return stable_; }

 private:
  uint32_t settle_ms_;
  bool pending_ = false;
  bool stable_ = false;
  uint64_t last_change_ms_ = 0;
  bool primed_ = false;
};

#if defined(CONFIG_HCP_INPUT) || defined(__ZEPHYR__)
// Target-side button bound to the devicetree `sw0` alias. Polls the gpio and
// drives a Debouncer; the callback fires on each settled edge.
class Button {
 public:
  using Callback = void (*)(bool pressed, void* arg);
  static bool Init(Callback cb, void* arg);
  // Current settled level (false before Init or when no button is defined).
  static bool Pressed();
};
#endif  // CONFIG_HCP_INPUT

// Pure short/long press classifier. Feed it the settled level on every tick (the
// app already runs one for the LED animation) - it needs repeat calls, because a
// long press must be reported while the button is still held, not on release.
//
//   * held >= long_ms          -> kLong, emitted once, immediately
//   * released before long_ms:
//       duration <= short_max_ms -> kShort
//       otherwise                -> kAborted (deliberate no-man's-land release)
//
// Keeping the thresholds in the constructor lets the policy live in the caller and
// the timing logic stay host-testable.
class PressClassifier {
 public:
  enum Action {
    kNone = 0,
    kShort,
    kLong,
    kAborted,
  };

  PressClassifier(uint32_t long_ms, uint32_t short_max_ms)
      : long_ms_(long_ms), short_max_ms_(short_max_ms) {}

  Action Update(bool pressed, uint64_t now_ms);
  bool held() const { return held_; }

 private:
  uint32_t long_ms_;
  uint32_t short_max_ms_;
  bool held_ = false;
  bool long_fired_ = false;
  uint64_t start_ms_ = 0;
};

}  // namespace input
}  // namespace hcp
