/*
 * hcp::input — Zephyr backend. Polls the `sw0` gpio-keys alias and feeds a
 * Debouncer; invokes the callback on each settled edge.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "hcp/input/input.h"

#ifdef CONFIG_HCP_INPUT

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>

#ifndef CONFIG_HCP_INPUT_DEBOUNCE_MS
#define CONFIG_HCP_INPUT_DEBOUNCE_MS 30
#endif
#define HCP_INPUT_POLL_MS 5

namespace hcp {
namespace input {

static const struct gpio_dt_spec g_btn =
    GPIO_DT_SPEC_GET_OR(DT_ALIAS(sw0), gpios, {0});
static Debouncer g_debouncer(CONFIG_HCP_INPUT_DEBOUNCE_MS);
static Button::Callback g_cb = nullptr;
static void* g_arg = nullptr;
static K_WORK_DELAYABLE_DEFINE(g_poll_work, nullptr);

static void PollHandler(struct k_work* /*work*/) {
  if (!gpio_is_ready_dt(&g_btn)) {
    return;
  }
  bool raw = gpio_pin_get_dt(&g_btn) > 0;  // dts marks buttons ACTIVE_LOW
  int ev = g_debouncer.Update(raw, static_cast<uint64_t>(k_uptime_get()));
  if (ev != Debouncer::kNone && g_cb) {
    g_cb(g_debouncer.stable(), g_arg);
  }
  k_work_reschedule(&g_poll_work, K_MSEC(HCP_INPUT_POLL_MS));
}

bool Button::Init(Callback cb, void* arg) {
  if (!gpio_is_ready_dt(&g_btn)) {
    return false;
  }
  if (gpio_pin_configure_dt(&g_btn, GPIO_INPUT) != 0) {
    return false;
  }
  g_cb = cb;
  g_arg = arg;
  k_work_init_delayable(&g_poll_work, PollHandler);
  k_work_schedule(&g_poll_work, K_MSEC(HCP_INPUT_POLL_MS));
  return true;
}

bool Button::Pressed() { return g_debouncer.stable(); }

}  // namespace input
}  // namespace hcp

#endif  // CONFIG_HCP_INPUT
