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
};
#endif  // CONFIG_HCP_INPUT

}  // namespace input
}  // namespace hcp
