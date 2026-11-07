/*
 * HCP LED strip abstraction (WS2812-class addressable strip).
 * CPU-side framebuffer (set/clear/brightness) is host-testable; Show() flushes
 * on the Zephyr backend via the `led_strip` API.
 *
 * NOTE (Zephyr v4.4.2): no Espressif RMT led_strip driver in-tree, so the
 * on-target flush is available but OFF by default. The app-visible API is
 * backend-agnostic. Google C++ Style naming.
 *
 * Copyright (c) 2026 maybetaken
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <cstdint>

namespace hcp {
namespace led {

struct Color {
  uint8_t r;
  uint8_t g;
  uint8_t b;
};

class Strip {
 public:
  explicit Strip(uint16_t count);
  ~Strip();
  Strip(const Strip&) = delete;
  Strip& operator=(const Strip&) = delete;

  bool Open();
  uint16_t Count() const { return count_; }

  void SetAll(Color c);
  void SetPixel(uint16_t index, Color c);  // out-of-range ignored
  bool GetPixel(uint16_t index, Color& out) const;

  void SetBrightness(uint8_t percent);  // 0..100
  uint8_t Brightness() const { return brightness_; }
  void Clear();

  bool Show();    // flush, unless the framebuffer already matches what was sent
  bool Refresh(); // flush whatever it holds, match or not - see Flush(bool)

  /* Frames handed to the wire since Open() (posix: Show()/Refresh() calls, where
   * the framebuffer is the whole model). The count exists because "the light is
   * showing the wrong thing" has two answers that look identical from outside -
   * this application put that frame there, or something else moved the data line -
   * and a device with no serial cable has no third way to say which. Read from the
   * MQTT session thread while the workqueue writes it: a sample, not a snapshot, and
   * no decision is made from it. */
  uint32_t Frames() const { return frames_; }

  /* Flushes the driver refused, and the code the last one gave. The pair is there to
   * be read over the network, because a strip that fails has exactly one observable
   * symptom - a pixel doing something odd - and its other two answers ("the driver
   * never reported done" and "the workqueue is spending half its time in the driver's
   * timeout") are console messages on a board nobody has cabled up. */
  uint32_t Errors() const { return errors_; }
  int LastError() const { return last_rc_; }

 private:
  /* The no-change check is a statement about this process, not about the pixel: a
   * strip holds whatever frame it last latched, for as long as it has power. So an
   * unchanged frame is not only redundant - for a light that is being held in some
   * state (off, above all, which has no edges left to send) it is also a claim that
   * the wire still agrees with us. Refresh() is how a caller stops making that claim:
   * re-assert, and let the driver overwrite what anything else may have latched. */
  bool Flush(bool force);

  struct Impl;
  Impl* impl_;
  uint16_t count_;
  bool open_ = false;
  uint8_t brightness_ = 100;
  uint32_t frames_ = 0;
  uint32_t errors_ = 0;
  int last_rc_ = 0;
};

}  // namespace led
}  // namespace hcp
