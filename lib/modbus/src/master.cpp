/*
 * hcp::modbus::Master — nanoMODBUS RTU client over hcp::serial::Port.
 * Google C++ Style naming.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "hcp/modbus/master.h"

#include "hcp/serial/port.h"

extern "C" {
#include "nanomodbus.h"
}

namespace hcp {
namespace modbus {

namespace {

// nmbs platform read/write: arg is a hcp::serial::Port*.
int32_t PlatformRead(uint8_t* buf, uint16_t count, int32_t byte_timeout_ms,
                     void* arg) {
  auto* port = static_cast<serial::Port*>(arg);
  uint32_t to = (byte_timeout_ms < 0) ? 0xFFFFFFFFu
                                      : static_cast<uint32_t>(byte_timeout_ms);
  return port->Read(buf, count, to);
}

int32_t PlatformWrite(const uint8_t* buf, uint16_t count,
                      int32_t /*byte_timeout_ms*/, void* arg) {
  auto* port = static_cast<serial::Port*>(arg);
  return port->Write(buf, count);
}

}  // namespace

struct Master::Impl {
  serial::Port port;
  nmbs_t nmbs;
  bool open = false;
};

Master::Master() : impl_(new Impl()) {}
Master::~Master() { Close(); delete impl_; }

bool Master::Open(const Config& cfg) {
  if (impl_->open) {
    return true;
  }
  if (!impl_->port.Open(cfg.baud)) {
    return false;
  }

  nmbs_platform_conf conf;
  nmbs_platform_conf_create(&conf);
  conf.transport = NMBS_TRANSPORT_RTU;
  conf.read = PlatformRead;
  conf.write = PlatformWrite;
  conf.arg = &impl_->port;

  nmbs_error err = nmbs_client_create(&impl_->nmbs, &conf);
  if (err != NMBS_ERROR_NONE) {
    last_error_ = static_cast<int>(err);
    impl_->port.Close();
    return false;
  }
  nmbs_set_read_timeout(&impl_->nmbs, cfg.response_timeout_ms);
  nmbs_set_byte_timeout(&impl_->nmbs, cfg.byte_timeout_ms);
  impl_->open = true;
  return true;
}

void Master::Close() {
  if (impl_->open) {
    impl_->port.Close();
    impl_->open = false;
  }
}

bool Master::ReadHoldingRegisters(uint8_t addr, uint16_t reg, uint16_t count,
                                  uint16_t* out) {
  if (!impl_->open) {
    last_error_ = -1;  // not open (sentinel; nmbs_error values are >= 0)
    return false;
  }
  nmbs_set_destination_rtu_address(&impl_->nmbs, addr);
  nmbs_error err = nmbs_read_holding_registers(&impl_->nmbs, reg, count, out);
  last_error_ = static_cast<int>(err);
  return err == NMBS_ERROR_NONE;
}

bool Master::ReadInputRegisters(uint8_t addr, uint16_t reg, uint16_t count,
                                uint16_t* out) {
  if (!impl_->open) {
    last_error_ = -1;  // not open (sentinel; nmbs_error values are >= 0)
    return false;
  }
  nmbs_set_destination_rtu_address(&impl_->nmbs, addr);
  nmbs_error err = nmbs_read_input_registers(&impl_->nmbs, reg, count, out);
  last_error_ = static_cast<int>(err);
  return err == NMBS_ERROR_NONE;
}

bool Master::WriteSingleRegister(uint8_t addr, uint16_t reg, uint16_t value) {
  if (!impl_->open) {
    last_error_ = -1;  // not open (sentinel; nmbs_error values are >= 0)
    return false;
  }
  nmbs_set_destination_rtu_address(&impl_->nmbs, addr);
  nmbs_error err = nmbs_write_single_register(&impl_->nmbs, reg, value);
  last_error_ = static_cast<int>(err);
  return err == NMBS_ERROR_NONE;
}

bool Master::WriteMultipleRegisters(uint8_t addr, uint16_t reg, uint16_t count,
                                    const uint16_t* values) {
  if (!impl_->open) {
    last_error_ = -1;  // not open (sentinel; nmbs_error values are >= 0)
    return false;
  }
  nmbs_set_destination_rtu_address(&impl_->nmbs, addr);
  nmbs_error err = nmbs_write_multiple_registers(&impl_->nmbs, reg, count, values);
  last_error_ = static_cast<int>(err);
  return err == NMBS_ERROR_NONE;
}

}  // namespace modbus
}  // namespace hcp
