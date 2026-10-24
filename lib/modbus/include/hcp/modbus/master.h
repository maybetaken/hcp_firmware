/*
 * HCP Modbus RTU master — a thin C++ facade over Zephyr's own Modbus client
 * (subsys/modbus). The subsys owns the RTU framing, the t3.5 inter-frame timing
 * and the RS485 transceiver direction line; the UART and DE pin come from the
 * `zephyr,modbus-serial` devicetree node (see boards/helios/common/hcp_base.dtsi
 * and the per-board *-rs485.overlay), not from this header.
 *
 * There is no third-party Modbus implementation behind it, which is what lets
 * this layer be published as hcp's own facade without a licensing or version
 * question attached. Google C++ Style naming.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <cstdint>

namespace hcp {
namespace modbus {

// Largest register block one request can carry. The subsys builds the ADU inside
// a CONFIG_MODBUS_BUFFER_SIZE byte buffer and the FC03/FC16 overhead (unit id, FC,
// address, quantity, byte count, CRC) costs about 9 bytes, so this is the point
// where a longer transfer has to be split into several requests.
constexpr uint16_t kMaxRegisters = (CONFIG_MODBUS_BUFFER_SIZE - 10) / 2;

struct Config {
  uint32_t baud = 9600;
  // How long the client waits for a response. There is no separate inter-byte
  // timeout: the subsys decides a frame is over from t3.5 at this baud, so the
  // knob the previous backend needed does not exist here (and is not needed).
  uint32_t response_timeout_ms = 1000;
};

// Single-master RTU client. Each call carries the destination unit address, so one
// instance serves every slave on the bus. Not reentrant: the subsys serialises
// requests per interface with its own mutex, but last_error() is per instance.
class Master {
 public:
  Master();
  ~Master();
  Master(const Master&) = delete;
  Master& operator=(const Master&) = delete;

  // Resolves the devicetree interface and configures it as an RTU client. Returns
  // false (with last_error() set) when the node is missing or the subsys rejects
  // the parameters.
  bool Open(const Config& cfg);
  void Close();

  // count must be 1..kMaxRegisters. `out` receives count registers.
  bool ReadHoldingRegisters(uint8_t addr, uint16_t reg, uint16_t count,
                            uint16_t* out);
  bool ReadInputRegisters(uint8_t addr, uint16_t reg, uint16_t count,
                          uint16_t* out);
  bool WriteSingleRegister(uint8_t addr, uint16_t reg, uint16_t value);
  bool WriteMultipleRegisters(uint8_t addr, uint16_t reg, uint16_t count,
                              const uint16_t* values);

  // 0 on success, otherwise the subsys' -errno (-ENOTCONN from the subsys means
  // the slave did not answer, -EIO a malformed ADU), or -1 if Open() never
  // succeeded, or -EFBIG if the request was too long for the ADU buffer.
  int last_error() const { return last_error_; }

 private:
  // Interface index to use, or -1 (with last_error() set) when not open.
  int Iface();

  struct Impl;
  Impl* impl_;
  int last_error_ = 0;
};

}  // namespace modbus
}  // namespace hcp
