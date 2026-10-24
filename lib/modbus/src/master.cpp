/*
 * hcp::modbus::Master — facade over Zephyr's in-tree Modbus RTU client.
 * Google C++ Style naming.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "hcp/modbus/master.h"

#include <errno.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/modbus/modbus.h>

#include "hcp/log/log.h"

namespace hcp {
namespace modbus {

namespace {

// The interface the subsys registered for the board's `zephyr,modbus-serial`
// node. Looked up by name (as the upstream sample does) instead of assuming it is
// interface 0, so adding a second port later does not silently address the wrong
// one.
#define HCP_MODBUS_NODE DT_COMPAT_GET_ANY_STATUS_OKAY(zephyr_modbus_serial)

int FindIface() {
  if (!DT_NODE_EXISTS(HCP_MODBUS_NODE)) {
    HCP_LOGE(hcp::log::kDomainModbus,
             "no zephyr,modbus-serial node: uart1 has no modbus child");
    return -1;
  }
  const char iface_name[] = {DEVICE_DT_NAME(HCP_MODBUS_NODE)};
  const int iface = modbus_iface_get_by_name(iface_name);
  if (iface < 0) {
    HCP_LOGE(hcp::log::kDomainModbus, "modbus interface '%s' not found (%d)",
             iface_name, iface);
  }
  return iface;
}

}  // namespace

struct Master::Impl {
  int iface = -1;
  bool open = false;
};

Master::Master() : impl_(new Impl()) {}
Master::~Master() {
  Close();
  delete impl_;
}

int Master::Iface() {
  if (!impl_->open) {
    last_error_ = -1;  // sentinel; the subsys' own failures are -errno
    return -1;
  }
  return impl_->iface;
}

bool Master::Open(const Config& cfg) {
  if (impl_->open) {
    return true;
  }
  impl_->iface = FindIface();
  if (impl_->iface < 0) {
    return false;
  }

  struct modbus_iface_param param = {
      .mode = MODBUS_MODE_RTU,
      .rx_timeout = cfg.response_timeout_ms * 1000U,  // the subsys counts µs
      .serial =
          {
              .baud = cfg.baud,
              // Modbus over serial line wants 8N1 to mean two stop bits when
              // parity is none; configure_uart() in the subsys applies that, so
              // only parity is passed here on purpose.
              .parity = UART_CFG_PARITY_NONE,
          },
  };

  const int err = modbus_init_client(impl_->iface, param);
  if (err != 0) {
    last_error_ = err;
    HCP_LOGE(hcp::log::kDomainModbus, "modbus_init_client failed (%d)", err);
    impl_->iface = -1;
    return false;
  }

  impl_->open = true;
  HCP_LOGI(hcp::log::kDomainModbus,
           "modbus RTU client up: iface %d, baud %u, response timeout %u ms",
           impl_->iface, static_cast<unsigned>(cfg.baud),
           static_cast<unsigned>(cfg.response_timeout_ms));
  return true;
}

void Master::Close() {
  if (!impl_->open) {
    return;
  }
  modbus_disable(static_cast<uint8_t>(impl_->iface));
  impl_->open = false;
  impl_->iface = -1;
}

bool Master::ReadHoldingRegisters(uint8_t addr, uint16_t reg, uint16_t count,
                                  uint16_t* out) {
  const int iface = Iface();
  if (iface < 0) {
    return false;
  }
  if (count == 0 || count > kMaxRegisters) {
    last_error_ = -EFBIG;
    return false;
  }
  last_error_ = modbus_read_holding_regs(iface, addr, reg, out, count);
  return last_error_ == 0;
}

bool Master::ReadInputRegisters(uint8_t addr, uint16_t reg, uint16_t count,
                                uint16_t* out) {
  const int iface = Iface();
  if (iface < 0) {
    return false;
  }
  if (count == 0 || count > kMaxRegisters) {
    last_error_ = -EFBIG;
    return false;
  }
  last_error_ = modbus_read_input_regs(iface, addr, reg, out, count);
  return last_error_ == 0;
}

bool Master::WriteCoil(uint8_t addr, uint16_t reg, bool on) {
  const int iface = Iface();
  if (iface < 0) {
    return false;
  }
  last_error_ = modbus_write_coil(iface, addr, reg, on);
  return last_error_ == 0;
}

bool Master::WriteSingleRegister(uint8_t addr, uint16_t reg, uint16_t value) {
  const int iface = Iface();
  if (iface < 0) {
    return false;
  }
  last_error_ = modbus_write_holding_reg(iface, addr, reg, value);
  return last_error_ == 0;
}

bool Master::WriteMultipleRegisters(uint8_t addr, uint16_t reg, uint16_t count,
                                    const uint16_t* values) {
  const int iface = Iface();
  if (iface < 0) {
    return false;
  }
  if (count == 0 || count > kMaxRegisters) {
    last_error_ = -EFBIG;
    return false;
  }
  // modbus_write_holding_regs() takes a non-const buffer (it reads the values out
  // of it while assembling the ADU), so copy rather than cast away the const of
  // our own API. kMaxRegisters is 123 with the default 256 byte ADU buffer.
  uint16_t buf[kMaxRegisters];
  memcpy(buf, values, static_cast<size_t>(count) * sizeof(buf[0]));
  last_error_ = modbus_write_holding_regs(iface, addr, reg, buf, count);
  return last_error_ == 0;
}

}  // namespace modbus
}  // namespace hcp
