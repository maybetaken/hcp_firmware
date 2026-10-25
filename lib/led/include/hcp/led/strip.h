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

  bool Show();  // flush; posix = no-op true

 private:
  struct Impl;
  Impl* impl_;
  uint16_t count_;
  bool open_ = false;
  uint8_t brightness_ = 100;
};

}  // namespace led
}  // namespace hcp
