/*
 * hcp::led — Zephyr backend. Framebuffer + flush, over either the led_strip API on
 * the `hcp-led-strip` devicetree alias (worldsemi,ws2812-spi) or, where the board
 * says so, bit-banging the data GPIO directly. Guarded so it is only built when a
 * Zephyr-side backend is chosen; the framebuffer, the brightness scaling and the
 * no-change check are shared, so the two paths cannot drift apart.
 * No STL (Zephyr minimal C++).
 * SPDX-License-Identifier: Apache-2.0
 */
#if defined(CONFIG_HCP_LED_BACKEND_ZEPHYR) || \
    defined(CONFIG_HCP_LED_BACKEND_GPIO_BITBANG)

#include "hcp/led/strip.h"

#include <zephyr/device.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/irq.h>

#include <string.h>

#if defined(CONFIG_HCP_LED_BACKEND_GPIO_BITBANG)
#include <zephyr/drivers/gpio.h>
namespace {

/* WS2812 bit cells in system-clock cycles. The clock here is 16 MHz, so one cycle
 * is 62.5 ns and the cells land at t0h=6, t1h=12, bit=20 - inside the datasheet's
 * 400/800/1250 ns with its +/-150 ns tolerance, and the same 1:2 ratio.
 *
 * Each edge waits for an absolute deadline rather than for a duration, so the cost
 * of the gpio call and of the loop itself is absorbed instead of accumulating down
 * the 24 bits of a pixel. */
constexpr uint32_t kCycPerUs = sys_clock_hw_cycles_per_sec() / 1000000U;
constexpr uint32_t kT0h = kCycPerUs * 4U / 10U;
constexpr uint32_t kT1h = kCycPerUs * 8U / 10U;
constexpr uint32_t kBitCell = kCycPerUs * 12U / 10U;

void WaitUntil(uint32_t base, uint32_t target) {
  while (static_cast<uint32_t>(sys_clock_cycle_get_32() - base) < target) {
  }
}

void BangByte(const gpio_dt_spec& led, uint8_t value, uint32_t base, uint32_t& cell) {
  for (int bit = 7; bit >= 0; bit--) {
    const bool one = ((value >> bit) & 1) != 0;
    WaitUntil(base, cell * kBitCell);
    gpio_pin_set_dt(&led, 1);
    WaitUntil(base, cell * kBitCell + (one ? kT1h : kT0h));
    gpio_pin_set_dt(&led, 0);
    cell++;
  }
}

}  // namespace
#endif  // GPIO_BITBANG

namespace hcp {
namespace led {

struct Strip::Impl {
  Color* fb = nullptr;
  Color* shown = nullptr;   // last frame actually sent, for the no-change check
  uint8_t shown_brightness = 0xFF;
  const struct device* dev = nullptr;
#if defined(CONFIG_HCP_LED_BACKEND_GPIO_BITBANG)
  gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_NODELABEL(led_0), gpios);
#endif
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
#if defined(CONFIG_HCP_LED_BACKEND_GPIO_BITBANG)
  /* One output line, idle low. There is no device to be "ready", and the frame is
   * written in software from here on. */
  if (!gpio_is_ready_dt(&impl_->led)) {
    return false;
  }
  if (gpio_pin_configure_dt(&impl_->led, GPIO_OUTPUT_INACTIVE) < 0) {
    return false;
  }
  open_ = true;
  return true;
#else
  impl_->dev = DEVICE_DT_GET(DT_ALIAS(hcp_led_strip));
  if (!device_is_ready(impl_->dev)) {
    impl_->dev = nullptr;
    return false;
  }
  open_ = true;
  return true;
#endif
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
#if defined(CONFIG_HCP_LED_BACKEND_GPIO_BITBANG)
  if (!open_) {
    return false;
  }
#else
  if (!open_ || impl_->dev == nullptr) {
    return false;
  }
#endif
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
#if defined(CONFIG_HCP_LED_BACKEND_GPIO_BITBANG)
  /* Green, red, blue - the WS2812 wire order, which the SPI binding expresses as a
   * color-mapping property and this path states directly.
   *
   * No latch wait at the end: the line is left low and stays low, so the >50 us
   * reset condition is met by the time the next frame begins, and spending it inside
   * irq_lock() would only extend an interrupt blackout on a board that also runs
   * BLE. */
  {
    const uint32_t base = sys_clock_cycle_get_32();
    uint32_t cell = 0;
    for (uint16_t i = 0; i < count_; ++i) {
      BangByte(impl_->led, rgb[i].g, base, cell);
      BangByte(impl_->led, rgb[i].r, base, cell);
      BangByte(impl_->led, rgb[i].b, base, cell);
    }
  }
  const int rc = 0;
#else
  int rc = led_strip_update_rgb(impl_->dev, rgb, count_);
#endif
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

#endif  // a Zephyr-side LED backend
