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

#include <string.h>

namespace hcp {
namespace led {

struct Strip::Impl {
  Color* fb = nullptr;
  const struct device* dev = nullptr;
};

Strip::Strip(uint16_t count) : impl_(new Impl()), count_(count) {
  impl_->fb = new Color[count ? count : 1];
  for (uint16_t i = 0; i < count; ++i) {
    impl_->fb[i] = Color{};
  }
}
Strip::~Strip() {
  delete[] impl_->fb;
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
  struct led_rgb* rgb = new struct led_rgb[count_];
  const uint32_t b = brightness_;
  for (uint16_t i = 0; i < count_; ++i) {
    rgb[i].r = static_cast<uint8_t>((impl_->fb[i].r * b) / 100u);
    rgb[i].g = static_cast<uint8_t>((impl_->fb[i].g * b) / 100u);
    rgb[i].b = static_cast<uint8_t>((impl_->fb[i].b * b) / 100u);
  }
  int rc = led_strip_update_rgb(impl_->dev, rgb, count_);
  delete[] rgb;
  return rc == 0;
}

}  // namespace led
}  // namespace hcp

#endif  // CONFIG_HCP_LED_BACKEND_ZEPHYR
