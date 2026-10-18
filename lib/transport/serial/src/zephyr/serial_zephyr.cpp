/*
 * hcp::serial — Zephyr backend (polled uart_api on the `hcp-uart` alias).
 * Optional RS485 half-duplex: if the devicetree alias `hcp-rs485-dir` exists,
 * Write() drives DE high before sending and low after the TX shift register
 * drains. On TTL boards (no such alias) this is compiled out entirely.
 * Google C++ Style naming.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "hcp/serial/port.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/devicetree.h>

namespace hcp {
namespace serial {

/* Present only on RS485 variants (overlay defines the hcp-rs485-dir alias). */
#if DT_NODE_EXISTS(DT_ALIAS(hcp_rs485_dir))
#define HCP_RS485 1
static const struct gpio_dt_spec g_de =
    GPIO_DT_SPEC_GET(DT_ALIAS(hcp_rs485_dir), gpios);
#else
#define HCP_RS485 0
#endif

struct Port::Impl {
  const struct device* dev = nullptr;
  uint32_t baud = 0;
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
#if HCP_RS485
  if (gpio_is_ready_dt(&g_de)) {
    gpio_pin_configure_dt(&g_de, GPIO_OUTPUT_INACTIVE);  // RX by default
  }
#endif
  impl_->baud = baud ? baud : 9600;
  impl_->open = true;
  return true;
}

void Port::Close() { impl_->open = false; }
bool Port::IsOpen() const { return impl_->open; }

int Port::Write(const uint8_t* data, size_t len) {
  if (!impl_->open || impl_->dev == nullptr) {
    return -1;
  }
#if HCP_RS485
  gpio_pin_set_dt(&g_de, 1);  // enable driver -> TX
  k_busy_wait(10);            // settle before first edge
#endif
  for (size_t i = 0; i < len; ++i) {
    uart_poll_out(impl_->dev, data[i]);  // returns void; blocking until sent
  }
#if HCP_RS485
  // Wait ~2 byte-times for the shift register to drain, then release to RX.
  const uint32_t byte_us = 10u * 1000000u / impl_->baud;
  k_busy_wait(2 * byte_us);
  gpio_pin_set_dt(&g_de, 0);
#endif
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
