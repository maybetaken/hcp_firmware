/*
 * HCP Modbus RTU master — thin wrapper over the vendored nanoMODBUS client,
 * transport provided by hcp::serial::Port (uart1). RS485 direction (DE) is
 * handled inside the serial backend when the `hcp-rs485-dir` alias is present.
 * Google C++ Style naming.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <cstdint>

namespace hcp {
namespace modbus {

struct Config {
  uint32_t baud = 9600;
  int32_t response_timeout_ms = 1000;  // wait for a full response
  int32_t byte_timeout_ms = 100;       // inter-byte gap = end of frame
};

// Single-master RTU client. Each call sets the destination unit address.
class Master {
 public:
  Master();
  ~Master();
  Master(const Master&) = delete;
  Master& operator=(const Master&) = delete;

  bool Open(const Config& cfg);
  void Close();

  // addr: Modbus unit/slave address (0 = broadcast, response not expected)
  bool ReadHoldingRegisters(uint8_t addr, uint16_t reg, uint16_t count,
                            uint16_t* out);
  bool ReadInputRegisters(uint8_t addr, uint16_t reg, uint16_t count,
                          uint16_t* out);
  bool WriteSingleRegister(uint8_t addr, uint16_t reg, uint16_t value);
  bool WriteMultipleRegisters(uint8_t addr, uint16_t reg, uint16_t count,
                              const uint16_t* values);

  int last_error() const { return last_error_; }  // nmbs_error value

 private:
  struct Impl;
  Impl* impl_;
  int last_error_ = 0;
};

}  // namespace modbus
}  // namespace hcp
