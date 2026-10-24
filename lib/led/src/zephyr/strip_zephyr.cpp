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

#include "hcp/log/log.h"

#if defined(CONFIG_HCP_LED_BACKEND_GPIO_BITBANG)
#include <zephyr/drivers/gpio.h>
namespace {

/*
 * WS2812 bit-banging on an ESP32-C3, timed by counting instructions.
 *
 * Why not by reading the clock: measured on the bench C3 (2.0.3.14) with the
 * interrupt locked, one sys_clock_cycle_get_32() costs 687 ns and one
 * gpio_pin_set_dt() 312 ns, while a whole bit period is 1250 ns and a zero's high
 * phase is 400 ns. The clock read is longer than the phase it would have to bound,
 * so a deadline-based loop cannot produce a valid frame however the constants are
 * tuned - and what it does produce is observable: every bit came out at the same
 * ~2000 ns floor (2 stores + 2 reads), which the strip reads as all-ones regardless
 * of the data. That is the "lit, but never blinks and will not switch off" failure,
 * and it is why the LED looked alive but ignored everything.
 *
 * So: one store per edge (gpio_port_set_bits_raw / clear_bits_raw, which are the
 * driver's own unlocked single-register writes - the 312 ns above is pin_set's lock
 * and flag checks, not the store), phases from a counted spin loop, and exactly two
 * clock reads in the whole lifetime path: one calibration run long enough (0.5 ms)
 * that the 687 ns granularity is 0.1 % rather than the whole budget.
 */
constexpr uint32_t kT0hNs = 400;   // datasheet 400 +/- 150
constexpr uint32_t kT1hNs = 800;   // datasheet 800 +/- 150
constexpr uint32_t kBitNs = 1250;  // period, so a one's low half is 450 ns

/* Keeps the loop body from being optimised away, and is the thing whose cost the
 * calibration measures. */
volatile uint32_t g_spin_sink;

void Spin(uint32_t n) {
  while (n != 0U) {
    n--;
    g_spin_sink = n;
  }
}

// Picoseconds per Spin(1) iteration, 0 until calibrated.
uint32_t g_ps_per_iter = 0;

bool CalibrateSpin() {
  constexpr uint32_t kIters = 20000;
  const uint32_t t0 = sys_clock_cycle_get_32();
  Spin(kIters);
  const uint32_t cycles = sys_clock_cycle_get_32() - t0;
  if (cycles == 0U) {
    return false;
  }
  g_ps_per_iter =
      static_cast<uint32_t>((static_cast<uint64_t>(cycles) * 1000000000000ULL) /
                            (static_cast<uint64_t>(sys_clock_hw_cycles_per_sec()) *
                             kIters));
  return g_ps_per_iter != 0U;
}

uint32_t ItersForNs(uint32_t ns) {
  if (g_ps_per_iter == 0U) {
    return 0U;
  }
  const uint32_t ps = ns * 1000U;
  return (ps + g_ps_per_iter / 2U) / g_ps_per_iter;
}

uint32_t NsOfIters(uint32_t iters) {
  /* Picoseconds per iteration, so nanoseconds is /1000 - and rounded, because this
   * feeds a log line that is the only evidence of the phases on a bench with no
   * scope. The first two versions of this were wrong in the same direction and both
   * were caught by that line reading t0H == t1H: /1000000 made it return microseconds
   * under a nanoseconds label, and truncation then reported a real 5-iteration
   * (220 ns) phase as 0. */
  return static_cast<uint32_t>(
      (static_cast<uint64_t>(iters) * g_ps_per_iter + 500ULL) / 1000ULL);
}

uint32_t CyclesToNs(uint32_t cycles) {
  return static_cast<uint32_t>(
      (static_cast<uint64_t>(cycles) * 1000000000ULL) /
      sys_clock_hw_cycles_per_sec());
}

/* One store, no lock, no argument checking: the same call gpio_esp32_port_set_bits_raw
 * makes internally. */
void Raise(const gpio_dt_spec& led) {
  gpio_port_set_bits_raw(led.port, BIT(led.pin));
}

void Lower(const gpio_dt_spec& led) {
  gpio_port_clear_bits_raw(led.port, BIT(led.pin));
}

/* How long one store takes, because it lands inside the high phase and has to come
 * out of the target. 4096 of them is tens of microseconds, which puts the 687 ns
 * clock granularity under 2 %; the batch is also what shows the store is cheap - the
 * 312 ns measured for gpio_pin_set_dt() was the driver's lock and checks, and if that
 * number came back here, this approach would still be out of budget and the answer
 * would be RMT. */
uint32_t MeasureStoreNs(const gpio_dt_spec& led) {
  constexpr uint32_t kStores = 4096;
  const uint32_t t0 = sys_clock_cycle_get_32();
  for (uint32_t i = 0; i < kStores; i++) {
    Raise(led);
  }
  const uint32_t cycles = sys_clock_cycle_get_32() - t0;
  return CyclesToNs(cycles / kStores);
}

/* A run of identical bits, with the falling-edge delay held at `low` for every one of
 * them. Used by the calibration self-test, where a constant bit cell would hide
 * exactly what is being measured. */
void SendUniform(const gpio_dt_spec& led, uint32_t hi, uint32_t low, uint32_t bits) {
  for (uint32_t i = 0; i < bits; i++) {
    Raise(led);
    Spin(hi);
    Lower(led);
    Spin(low);
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
  /* Spin iterations for the four timings, derived from the boot calibration. Kept
   * per-pixel-instance rather than recomputed because Show() runs with interrupts
   * locked and should not be doing 64-bit division there. */
  uint32_t hi0 = 0, hi1 = 0, lo0 = 0, lo1 = 0;
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

  /* Calibrate once, here, before anything can be displayed. */
  if (!CalibrateSpin()) {
    HCP_LOGW(hcp::log::kDomainDevice, "led: spin calibration failed, giving up");
    return false;
  }
  const unsigned int key = irq_lock();
  const uint32_t store_ns = MeasureStoreNs(impl_->led);
  irq_unlock(key);
  /* The store happens before the line is high and the next one before it is low
   * again, so each high phase is the target minus one store. */
  impl_->hi0 = ItersForNs(kT0hNs > store_ns ? kT0hNs - store_ns : 0U);
  impl_->hi1 = ItersForNs(kT1hNs > store_ns ? kT1hNs - store_ns : 0U);
  impl_->lo0 = ItersForNs(kBitNs - kT0hNs);
  impl_->lo1 = ItersForNs(kBitNs - kT1hNs);

  /* Prove the two phases really differ, without a scope: hold the falling-edge delay
   * fixed and send 24 bits at the one phase, then 24 at the zero phase. The delta is
   * then 24 x (t1H - t0H) = 9600 ns, and the two clock reads bound it by about
   * +/- 1.4 us - enough to tell "a one is wider than a zero" from the previous
   * approach's failure, where every bit came out at the same 2000 ns floor and the
   * strip could not read a single one of them. */
  const uint32_t low = impl_->lo0;
  const unsigned int test_key = irq_lock();
  const uint32_t t0 = sys_clock_cycle_get_32();
  SendUniform(impl_->led, impl_->hi0, low, 24);
  const uint32_t t1 = sys_clock_cycle_get_32();
  SendUniform(impl_->led, impl_->hi1, low, 24);
  const uint32_t t2 = sys_clock_cycle_get_32();
  irq_unlock(test_key);
  HCP_LOGI(hcp::log::kDomainDevice,
           "led bit-bang: store %u ns, spin %u ns/iter, t0H %u ns t1H %u ns, "
           "24-bit delta %u ns (want ~9600)",
           static_cast<unsigned>(store_ns),
           static_cast<unsigned>(g_ps_per_iter / 1000U),
           static_cast<unsigned>(store_ns + NsOfIters(impl_->hi0)),
           static_cast<unsigned>(store_ns + NsOfIters(impl_->hi1)),
           static_cast<unsigned>(CyclesToNs(t2 - t1) - CyclesToNs(t1 - t0)));
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
    /* Deliberately silent, and measured before deciding: this is the normal path -
     * the animation tick repaints every 50 ms and 19 times in 20 the colour did not
     * change - so logging it puts twenty lines a second on the console and overflows
     * the 1 KB log ring on the ESP32-C5, which would drop the boot messages that
     * matter. The flush line below is the one worth having, because it is rare and it
     * is the actual bytes that left for the strip. */
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
  for (uint16_t i = 0; i < count_; ++i) {
    const uint8_t bytes[3] = {rgb[i].g, rgb[i].r, rgb[i].b};
    for (uint8_t b = 0; b < 3; b++) {
      for (int bit = 7; bit >= 0; bit--) {
        const bool one = ((bytes[b] >> bit) & 1U) != 0U;
        Raise(impl_->led);
        Spin(one ? impl_->hi1 : impl_->hi0);
        Lower(impl_->led);
        /* The low half is the rest of the 1250 ns cell, so a one and a zero take the
         * same time and the strip's frame length depends only on the bit count. */
        Spin(one ? impl_->lo1 : impl_->lo0);
      }
    }
  }
  const int rc = 0;
#else
  int rc = led_strip_update_rgb(impl_->dev, rgb, count_);
#endif
  irq_unlock(key);

  if (rc == 0) {
    memcpy(impl_->shown, impl_->fb, sizeof(Color) * count_);
    impl_->shown_brightness = brightness_;
    /* Read while `rgb` is still alive: these are the bytes that actually went out,
     * after the brightness scaling, which is the difference between "the application
     * asked for violet" and "the strip was told rgb(17,0,25) at 10 %". */
    HCP_LOGD(hcp::log::kDomainDevice, "led flush rgb(%u,%u,%u) at %u%% -> %s",
             static_cast<unsigned>(rgb[0].r), static_cast<unsigned>(rgb[0].g),
             static_cast<unsigned>(rgb[0].b), static_cast<unsigned>(brightness_),
             impl_->dev != nullptr ? "led_strip" : "bit-bang");
  }
  delete[] rgb;
  return rc == 0;
}

}  // namespace led
}  // namespace hcp

#endif  // a Zephyr-side LED backend
