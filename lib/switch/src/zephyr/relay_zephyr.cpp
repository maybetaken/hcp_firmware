/*
 * hcp::switchctl — Zephyr backend (drives relay gpios hcp-k1/hcp-k2).
 * Copyright (c) 2026 maybetaken
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "hcp/switch/relay.h"

#include <zephyr/drivers/gpio.h>

namespace hcp {
namespace switchctl {

namespace {
const struct gpio_dt_spec kRelayGpios[kRelayCount] = {
    GPIO_DT_SPEC_GET_OR(DT_ALIAS(hcp_k1), gpios, {0}),
    GPIO_DT_SPEC_GET_OR(DT_ALIAS(hcp_k2), gpios, {0}),
};
}  // namespace

struct Relay::Impl {
  struct gpio_dt_spec spec;
  bool bound = false;
};

Relay::Relay() : impl_(new Impl()) {}
Relay::~Relay() { delete impl_; }

bool Relay::Open(int index) {
  if (index < 0 || index >= kRelayCount) {
    return false;
  }
  const struct gpio_dt_spec& spec = kRelayGpios[index];
  if (!gpio_is_ready_dt(&spec)) {
    return false;
  }
  if (gpio_pin_configure_dt(&spec, GPIO_OUTPUT_INACTIVE) != 0) {
    return false;
  }
  impl_->spec = spec;
  impl_->bound = true;
  index_ = index;
  state_ = false;
  return true;
}

void Relay::Set(bool on) {
  if (!impl_->bound) {
    return;
  }
  gpio_pin_set_dt(&impl_->spec, on ? 1 : 0);
  state_ = on;
}

bool Relay::Get() const { return state_; }

}  // namespace switchctl
}  // namespace hcp
