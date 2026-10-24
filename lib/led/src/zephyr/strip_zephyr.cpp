/*
 * hcp::led - Zephyr backend: framebuffer plus flush through the led_strip API.
 *
 * The board decides which driver sits behind the `hcp-led-strip` alias - the
 * espressif,esp32-ws2812-rmt driver on all three Helios boards - and nothing here asks.
 * That indirection is the reason the C3's timing fix lives in
 * modules/drivers/ws2812_rmt instead of in this file: an application facade that grows
 * a #ifdef per SoC ends up knowing more about the hardware than the drivers do, and the
 * shared parts (the framebuffer, the brightness scaling, the "do not resend an unchanged
 * frame" check below) start to differ between branches.
 *
 * No STL (Zephyr minimal C++). Google C++ Style naming.
 * SPDX-License-Identifier: Apache-2.0
 */
#if defined(CONFIG_HCP_LED_BACKEND_ZEPHYR)

#include "hcp/led/strip.h"

#include <zephyr/device.h>
#include <zephyr/drivers/led_strip.h>

#include <string.h>

#include "hcp/log/log.h"

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

  /* A blink only changes the framebuffer on its edges, so skipping an unchanged
   * frame keeps the driver's work down to the few per second that actually alter the
   * strip rather than every animation tick. */
  bool same = impl_->shown_brightness == brightness_;
  for (uint16_t i = 0; same && i < count_; ++i) {
    const Color& want = impl_->fb[i];
    const Color& sent = impl_->shown[i];
    same = want.r == sent.r && want.g == sent.g && want.b == sent.b;
  }
  if (same) {
    /* Silent on purpose: this is the normal path (the animation tick repaints every
     * 50 ms and almost always with the same colour), and logging it costs twenty
     * lines a second - enough on the ESP32-C5's 1 KB log ring to drop the boot
     * messages that matter. */
    return true;
  }

  struct led_rgb* rgb = new struct led_rgb[count_];
  const uint32_t scale = brightness_;
  for (uint16_t i = 0; i < count_; ++i) {
    rgb[i].r = static_cast<uint8_t>((impl_->fb[i].r * scale) / 100u);
    rgb[i].g = static_cast<uint8_t>((impl_->fb[i].g * scale) / 100u);
    rgb[i].b = static_cast<uint8_t>((impl_->fb[i].b * scale) / 100u);
  }

  /* No critical section around the flush. The SPI route required one: a
   * worldsemi,ws2812-spi frame tolerates no gap inside it, because a pause of a few
   * tens of microseconds reads as the latch condition and the pixels re-sync on the
   * remaining bits - seen on the C6 as a stray green flash while violet was intended.
   * RMT has no such constraint (the peripheral owns the waveform once started, so an
   * ISR delays a frame rather than corrupting a bit inside one), and keeping the lock
   * would cost more than it buys: this call busy-waits on the hardware through a chunk
   * and then holds the line low to latch, so on the failure it is built to report - a
   * channel that never finishes - interrupts would stay locked for the whole timeout
   * while the radio and the 485 driver were still expected to run. */
  const int rc = led_strip_update_rgb(impl_->dev, rgb, count_);

  if (rc == 0) {
    memcpy(impl_->shown, impl_->fb, sizeof(Color) * count_);
    impl_->shown_brightness = brightness_;
    /* The bytes that actually went out, after scaling: the difference between "the
     * application asked for violet" and "the strip was told rgb(17,0,25) at 10 %",
     * which is the question to ask when a colour looks wrong. */
    HCP_LOGD(hcp::log::kDomainDevice, "led flush rgb(%u,%u,%u) at %u%%",
             static_cast<unsigned>(rgb[0].r), static_cast<unsigned>(rgb[0].g),
             static_cast<unsigned>(rgb[0].b), static_cast<unsigned>(brightness_));
  } else {
    HCP_LOGW(hcp::log::kDomainDevice, "led flush failed (%d)", rc);
  }
  delete[] rgb;
  return rc == 0;
}

}  // namespace led
}  // namespace hcp

#endif  // CONFIG_HCP_LED_BACKEND_ZEPHYR
