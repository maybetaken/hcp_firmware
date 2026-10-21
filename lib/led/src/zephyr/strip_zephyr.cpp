/*
 * hcp::led — Zephyr backend. Framebuffer + flush via the `led_strip` API on the
 * `hcp-led-strip` devicetree alias. Guarded so it is only built when
 * CONFIG_HCP_LED_BACKEND_ZEPHYR is enabled (needs a ws2812 overlay, e.g.
 * lib/led/ws2812-spi.overlay). No STL (Zephyr minimal C++).
 * SPDX-License-Identifier: Apache-2.0
 */
#ifdef CONFIG_HCP_LED_BACKEND_ZEPHYR

#include "hcp/led/strip.h"

#include <zephyr/device.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/irq.h>

#include <string.h>

namespace hcp {
namespace led {

struct Strip::Impl {
  Color* fb = nullptr;
  Color* shown = nullptr;   // last frame actually sent, for the no-change check
  uint8_t shown_brightness = 0xFF;
  const struct device* dev = nullptr;
};

Strip::Strip(uint16_t count) : impl_(new Impl()), count_(count) {
  impl_->fb = new Color[count ? count : 1];
  impl_->shown = new Color[count ? count : 1];
  for (uint16_t i = 0; i < count; ++i) {
    impl_->fb[i] = Color{};
    impl_->shown[i] = Color{};
  }
}
Strip::~Strip() {
  delete[] impl_->fb;
  delete[] impl_->shown;
  delete impl_;
}

bool Strip::Open() {
  impl_->dev = DEVICE_DT_GET(DT_ALIAS(hcp_led_strip));
  if (!device_is_ready(impl_->dev)) {
    impl_->dev = nullptr;
    return false;
  }
  open_ = true;
  return true;
}

void Strip::SetAll(Color c) {
  for (uint16_t i = 0; i < count_; ++i) {
    impl_->fb[i] = c;
  }
}

void Strip::SetPixel(uint16_t index, Color c) {
  if (index < count_) {
    impl_->fb[index] = c;
  }
}

bool Strip::GetPixel(uint16_t index, Color& out) const {
  if (index >= count_) {
    return false;
  }
  out = impl_->fb[index];
  return true;
}

void Strip::SetBrightness(uint8_t percent) {
  brightness_ = percent > 100 ? 100 : percent;
}

void Strip::Clear() {
  for (uint16_t i = 0; i < count_; ++i) {
    impl_->fb[i] = Color{};
  }
}

bool Strip::Show() {
  if (!open_ || impl_->dev == nullptr) {
    return false;
  }
  /* Bit-banged WS2812 (worldsemi,ws2812-spi) tolerates no gap inside a frame: a
   * pause of a few tens of us is read as the latch condition, the pixels re-sync on
   * the remaining bits and the colours come out shifted (observed as a stray green
   * flash while the intended value was violet). Zephyr's driver takes no critical
   * section of its own, so hold interrupts off across the transfer.
   *
   * A blink only changes the framebuffer on its edges, so the no-change check
   * below keeps the interrupt-off windows to the few per second that actually
   * alter the strip instead of every animation tick. */
  bool same = impl_->shown_brightness == brightness_;
  for (uint16_t i = 0; same && i < count_; ++i) {
    const Color& want = impl_->fb[i];
    const Color& sent = impl_->shown[i];
    same = want.r == sent.r && want.g == sent.g && want.b == sent.b;
  }
  if (same) {
    return true;
  }

  struct led_rgb* rgb = new struct led_rgb[count_];
  const uint32_t scale = brightness_;
  for (uint16_t i = 0; i < count_; ++i) {
    rgb[i].r = static_cast<uint8_t>((impl_->fb[i].r * scale) / 100u);
    rgb[i].g = static_cast<uint8_t>((impl_->fb[i].g * scale) / 100u);
    rgb[i].b = static_cast<uint8_t>((impl_->fb[i].b * scale) / 100u);
  }
  const unsigned int key = irq_lock();
  int rc = led_strip_update_rgb(impl_->dev, rgb, count_);
  irq_unlock(key);
  delete[] rgb;

  if (rc == 0) {
    memcpy(impl_->shown, impl_->fb, sizeof(Color) * count_);
    impl_->shown_brightness = brightness_;
  }
  return rc == 0;
}

}  // namespace led
}  // namespace hcp

#endif  // CONFIG_HCP_LED_BACKEND_ZEPHYR
