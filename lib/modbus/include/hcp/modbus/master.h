/*
 * HCP Modbus RTU master — one facade, two backends.
 *
 *   subsys  (default, and what c5/c6 run): Zephyr's own subsys/modbus. It owns the
 *           RTU framing, t3.5 timing and the DE line; the UART and DE pin come from
 *           the `zephyr,modbus-serial` devicetree node (see
 *           boards/helios/common/hcp_base.dtsi and the per-board *-rs485.overlay).
 *   native  (c3): hcp's own frame engine over hcp::serial::Port, in rtu.h. Chosen
 *           per board because the subsys' idle-line framing cannot make this one
 *           meter's 200-480 ms turnaround work, and it gives no way to see the
 *           bytes it rejected. See rtu.h for the measurements.
 *
 * Either way there is no third-party Modbus implementation linked in, which is what
 * lets this layer be published as hcp's own facade without a licensing or version
 * question attached. Google C++ Style naming.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <cstdint>

#include "hcp/modbus/rtu.h"

namespace hcp {
namespace modbus {

// Largest register block one request can carry. Both backends are bounded by the
// same 256-byte ADU: the subsys builds it inside CONFIG_MODBUS_BUFFER_SIZE, the
// native engine inside rtu.h's kMaxRegsPerReply, which is derived from the same
// arithmetic (6 bytes of header + 2 CRC + 2 per register).
#if defined(CONFIG_HCP_MODBUS_BACKEND_NATIVE)
constexpr uint16_t kMaxRegisters = kMaxRegsPerReply;
#else
constexpr uint16_t kMaxRegisters = (CONFIG_MODBUS_BUFFER_SIZE - 10) / 2;
#endif

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
  // FC05. The subsys takes the state as a bool, so the 0xFF00/0x0000 wire encoding
  // is decoded before this is called (see registry::ParseWriteRequest).
  bool WriteCoil(uint8_t addr, uint16_t reg, bool on);
  bool WriteMultipleRegisters(uint8_t addr, uint16_t reg, uint16_t count,
                              const uint16_t* values);

  // 0 on success, otherwise the subsys' -errno (-ENOTCONN from the subsys means
  // the slave did not answer, -EIO a malformed ADU), or -1 if Open() never
  // succeeded, or -EFBIG if the request was too long for the ADU buffer.
  int last_error() const { return last_error_; }

 private:
  struct Impl;
  Impl* impl_;
  int last_error_ = 0;

#if defined(CONFIG_HCP_MODBUS_BACKEND_NATIVE)
  /* Native backend: the frame engine in rtu.cpp over hcp::serial::Port. */
  bool Iface();
  bool Transceive(const Request& req, Reply* rep, const char* what,
                  uint16_t count);
  bool ReadBlock(uint8_t addr, uint16_t reg, uint16_t count, uint16_t* out,
                 uint8_t fc, const char* what);
#else
  // Interface index to use, or -1 (with last_error() set) when not open.
  int Iface();
#endif
};

}  // namespace modbus
}  // namespace hcp
