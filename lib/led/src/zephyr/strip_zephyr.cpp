/*
 * hcp::led - Zephyr backend: framebuffer plus flush through the led_strip API.
 *
 * The driver behind the `hcp-led-strip` alias is a devicetree choice and nothing here
 * asks which one it is. That is why the timing work lives in modules/drivers/ws2812_rmt:
 * a facade that grows a #ifdef per SoC ends up knowing more about the hardware than the
 * drivers do, and the shared parts below start to differ between branches.
 *
 * No STL (Zephyr minimal C++). Google C++ Style naming.
 * Copyright (c) 2026 maybetaken
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#if defined(CONFIG_HCP_LED_BACKEND_ZEPHYR)

#include "hcp/led/strip.h"

#include <zephyr/device.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/kernel.h>  // k_uptime_get, for the failure backoff

#include <string.h>

#include "hcp/log/log.h"

namespace hcp {
namespace led {

/* How long to keep off a driver that just refused a frame, and how much longer each
 * further refusal adds. A failed update_rgb is not a quiet no-op: the driver reaches
 * its 200000-pass timeout before giving up - tens of milliseconds of busy-wait on the
 * cooperative system workqueue, which is also where the button debounce and the BluFi
 * receive work run - and it leaves whatever partial waveform it got going on the data
 * line for the strip to latch. Retrying that every 50 ms tick buys another timeout and
 * another partial frame, and measures out as an animation rate that is half what the
 * caller asked for. One success clears the window, which is also how a strip that came
 * back tells anyone without being asked. */
constexpr uint32_t kRetryBaseMs = 1000;
constexpr uint32_t kRetryCapMs = 30000;

struct Strip::Impl {
  Color* fb = nullptr;
  Color* shown = nullptr;   // last frame actually sent, for the no-change check
  uint8_t shown_brightness = 0xFF;
  const struct device* dev = nullptr;
  uint32_t retry_ms = 0;    // current backoff window; 0 means the last flush worked
  int64_t retry_at_ms = 0;  // and until when it holds
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

bool Strip::Show() { return Flush(false); }
bool Strip::Refresh() { return Flush(true); }

bool Strip::Flush(bool force) {
  if (!open_ || impl_->dev == nullptr) {
    return false;
  }

  /* Before the no-change check, and for a forced call too: "re-assert" asks the driver
   * for the frame it is currently refusing, and asking early only buys another
   * timeout. */
  const int64_t now = k_uptime_get();
  if (impl_->retry_ms != 0 && now < impl_->retry_at_ms) {
    return false;
  }

  /* A blink only changes the framebuffer on its edges, so skipping an unchanged
   * frame keeps the driver's work down to the few per second that actually alter the
   * strip rather than every animation tick. Refresh() is the caller saying "that
   * reasoning is about my framebuffer, and the pixel is not my framebuffer". */
  bool same = !force && impl_->shown_brightness == brightness_;
  for (uint16_t i = 0; same && i < count_; ++i) {
    const Color& want = impl_->fb[i];
    const Color& sent = impl_->shown[i];
    same = want.r == sent.r && want.g == sent.g && want.b == sent.b;
  }
  if (same) {
    /* Silent: this is the normal path (the animation ticks every 50 ms, almost always
     * with the same colour), and twenty log lines a second fills the C5's 1 KB ring. */
    return true;
  }

  struct led_rgb* rgb = new struct led_rgb[count_];
  const uint32_t scale = brightness_;
  for (uint16_t i = 0; i < count_; ++i) {
    rgb[i].r = static_cast<uint8_t>((impl_->fb[i].r * scale) / 100u);
    rgb[i].g = static_cast<uint8_t>((impl_->fb[i].g * scale) / 100u);
    rgb[i].b = static_cast<uint8_t>((impl_->fb[i].b * scale) / 100u);
  }

  /* No critical section: RMT owns the waveform once started, so an ISR delays a frame
   * rather than corrupting a bit inside one. A blackout here would also run this call's
   * busy-wait - and its timeout, on a channel that never finishes - with interrupts off. */
  const int rc = led_strip_update_rgb(impl_->dev, rgb, count_);
  /* Counted at the call, not at the success: the wire moved either way, and "how many
   * frames did we put on it" is the question this number answers. A failed flush is
   * reported on the line below it rather than hidden by a smaller count. */
  frames_++;
  last_rc_ = rc;

  if (rc == 0) {
    impl_->retry_ms = 0;
    memcpy(impl_->shown, impl_->fb, sizeof(Color) * count_);
    impl_->shown_brightness = brightness_;
    /* The bytes that actually went out, after scaling: the difference between "the
     * application asked for violet" and "the strip was told rgb(17,0,25) at 10 %",
     * which is the question to ask when a colour looks wrong. */
    HCP_LOGD(hcp::log::kDomainDevice, "led flush rgb(%u,%u,%u) at %u%%",
             static_cast<unsigned>(rgb[0].r), static_cast<unsigned>(rgb[0].g),
             static_cast<unsigned>(rgb[0].b), static_cast<unsigned>(brightness_));
  } else {
    /* Both numbers are counted here rather than left to the log because a strip that
     * fails never stops being asked: frames_ climbing while nothing changes is what a
     * refusal looks like from the network, and errors_/last_rc_ say it in words. */
    errors_++;
    impl_->retry_ms = impl_->retry_ms == 0
                         ? kRetryBaseMs
                         : (impl_->retry_ms >= kRetryCapMs / 2 ? kRetryCapMs
                                                               : impl_->retry_ms * 2);
    impl_->retry_at_ms = now + impl_->retry_ms;
    HCP_LOGW(hcp::log::kDomainDevice, "led flush failed (%d), retrying in %u ms", rc,
             static_cast<unsigned>(impl_->retry_ms));
  }
  delete[] rgb;
  return rc == 0;
}

}  // namespace led
}  // namespace hcp

#endif  // CONFIG_HCP_LED_BACKEND_ZEPHYR
