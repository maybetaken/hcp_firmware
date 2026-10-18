/*
 * hcp::serial — Zephyr backend (polled uart_api on the `hcp-uart` alias).
 * Google C++ Style naming.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "hcp/serial/port.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>

namespace hcp {
namespace serial {

struct Port::Impl {
  const struct device* dev = nullptr;
  bool open = false;
};

Port::Port() : impl_(new Impl()) {}
Port::~Port() { delete impl_; }

bool Port::Open(uint32_t baud) {
  impl_->dev = DEVICE_DT_GET(DT_ALIAS(hcp_uart));
  if (!device_is_ready(impl_->dev)) {
    impl_->dev = nullptr;
    return false;
  }
  const struct uart_config cfg = {
      .baudrate = static_cast<uint32_t>(baud),
      .parity = UART_CFG_PARITY_NONE,
      .stop_bits = UART_CFG_STOP_BITS_1,
      .data_bits = UART_CFG_DATA_BITS_8,
      .flow_ctrl = UART_CFG_FLOW_CTRL_NONE,
  };
  if (uart_configure(impl_->dev, &cfg) != 0) {
    impl_->dev = nullptr;
    return false;
  }
  impl_->open = true;
  return true;
}

void Port::Close() { impl_->open = false; }
bool Port::IsOpen() const { return impl_->open; }

int Port::Write(const uint8_t* data, size_t len) {
  if (!impl_->open || impl_->dev == nullptr) {
    return -1;
  }
  for (size_t i = 0; i < len; ++i) {
    uart_poll_out(impl_->dev, data[i]);  // returns void; blocking until sent
  }
  return static_cast<int>(len);
}

int Port::Read(uint8_t* buf, size_t cap, uint32_t timeout_ms) {
  if (!impl_->open || impl_->dev == nullptr || cap == 0) {
    return -1;
  }
  uint64_t deadline = k_uptime_get() + timeout_ms;
  size_t n = 0;
  while (n < cap) {
    unsigned char c;
    if (uart_poll_in(impl_->dev, &c) == 0) {
      buf[n++] = c;
      continue;
    }
    if (k_uptime_get() >= deadline) {
      break;
    }
    k_busy_wait(50);
  }
  return static_cast<int>(n);
}

}  // namespace serial
}  // namespace hcp
