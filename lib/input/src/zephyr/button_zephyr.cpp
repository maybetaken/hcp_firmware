/*
 * hcp::input — Zephyr backend. The `sw0` gpio-keys pin interrupts on both edges;
 * the ISR does nothing but reschedule a debounce work item, which samples the line
 * in thread context and feeds the same Debouncer the host tests cover. The callback
 * fires on each settled edge.
 *
 * Interrupt rather than polling because there is nothing worth polling: a button is
 * either held or not, and the previous 5 ms sampler ran on the system workqueue 200
 * times a second to notice a handful of events per day. The idle device now does no
 * work at all for the button. This collector never sleeps, so the pin is
 * deliberately not set up as a wakeup source (that would be GPIO_INT_WAKEUP on top
 * of the edge configuration below).
 *
 * Copyright (c) 2026 maybetaken
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "hcp/input/input.h"

#ifdef CONFIG_HCP_INPUT

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>

#include "hcp/log/log.h"

#ifndef CONFIG_HCP_INPUT_DEBOUNCE_MS
#define CONFIG_HCP_INPUT_DEBOUNCE_MS 30
#endif

namespace hcp {
namespace input {

namespace {

/* gpio-keys carries the pull direction in devicetree (GPIO_PULL_UP |
 * GPIO_ACTIVE_LOW), so gpio_pin_get_dt() already reports "pressed" as true. */
const struct gpio_dt_spec g_btn =
    GPIO_DT_SPEC_GET_OR(DT_ALIAS(sw0), gpios, {0});
struct gpio_callback g_cb_data;
Debouncer g_debouncer(CONFIG_HCP_INPUT_DEBOUNCE_MS);
K_WORK_DELAYABLE_DEFINE(g_debounce_work, nullptr);
Button::Callback g_cb = nullptr;
void* g_arg = nullptr;

/* ISR context. The bouncing pin is not sampled here: every edge just restarts the
 * settle window, so the line is read once the last one has passed. Same shape as
 * Zephyr's own gpio-keys driver. */
void EdgeIsr(const struct device* dev, struct gpio_callback* cb, uint32_t pins) {
  ARG_UNUSED(dev);
  ARG_UNUSED(cb);
  ARG_UNUSED(pins);
  k_work_reschedule(&g_debounce_work, K_MSEC(CONFIG_HCP_INPUT_DEBOUNCE_MS));
}

/* Thread context (system workqueue): take the reading and report settled edges. */
void DebounceWork(struct k_work* /*work*/) {
  const bool raw = gpio_pin_get_dt(&g_btn) > 0;
  const int ev = g_debouncer.Update(raw, static_cast<uint64_t>(k_uptime_get()));
  if (ev != Debouncer::kNone) {
    /* Every settled edge, at debug. "The button does nothing" splits in two here:
     * no line at all when the pin is pressed means the interrupt never fires (wrong
     * pin, nothing wired, pull configured against the board); a line that appears
     * but produces no short/long action means the tick that samples it is the
     * problem, not the input. */
    HCP_LOGD(hcp::log::kDomainDevice, "input: button settled %s (raw %d)",
             g_debouncer.stable() ? "pressed" : "released", raw ? 1 : 0);
    if (g_cb != nullptr) {
      g_cb(g_debouncer.stable(), g_arg);
    }
  }
  /* Update() commits a change only once it has seen the new level twice with the
   * settle window in between - so the first look at it must schedule another.
   * Without this an interrupt-driven caller would never report an edge. */
  if (ev == Debouncer::kNone && g_debouncer.settling()) {
    k_work_reschedule(&g_debounce_work, K_MSEC(CONFIG_HCP_INPUT_DEBOUNCE_MS));
  }
}

}  // namespace

bool Button::Init(Callback cb, void* arg) {
  if (!gpio_is_ready_dt(&g_btn)) {
    return false;
  }
  if (gpio_pin_configure_dt(&g_btn, GPIO_INPUT) != 0) {
    return false;
  }

  /* Seed the debouncer with the level at boot so a button held when power comes up
   * is not reported as a fresh press. */
  const bool boot_raw = gpio_pin_get_dt(&g_btn) > 0;
  g_debouncer.Update(boot_raw, static_cast<uint64_t>(k_uptime_get()));

  /* The idle reading is worth a line, because it is the one measurement that says
   * whether the pin can work at all: with the DT's GPIO_ACTIVE_LOW applied, a free
   * line reads "not pressed". If it reads pressed with nobody touching it, the
   * debouncer will be waiting for a release that never comes and no press event can
   * ever be produced - which looks exactly like a dead button. */
  HCP_LOGD(hcp::log::kDomainDevice, "input: button pin %d idle raw %d (ready %d)",
           g_btn.pin, gpio_pin_get(g_btn.port, g_btn.pin) > 0 ? 1 : 0,
           gpio_is_ready_dt(&g_btn) ? 1 : 0);

  g_cb = cb;
  g_arg = arg;
  k_work_init_delayable(&g_debounce_work, DebounceWork);

  gpio_init_callback(&g_cb_data, EdgeIsr, BIT(g_btn.pin));
  if (gpio_add_callback_dt(&g_btn, &g_cb_data) != 0) {
    return false;
  }
  if (gpio_pin_interrupt_configure_dt(&g_btn, GPIO_INT_EDGE_BOTH) != 0) {
    gpio_remove_callback_dt(&g_btn, &g_cb_data);
    return false;
  }
  return true;
}

bool Button::Pressed() { return g_debouncer.stable(); }

}  // namespace input
}  // namespace hcp

#endif  // CONFIG_HCP_INPUT
