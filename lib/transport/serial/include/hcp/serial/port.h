/*
 * HCP serial transport — the Modbus UART (devicetree alias `hcp-uart`).
 * A thin byte-pipe over the target UART; RS485 direction (DE) is layered on
 * when the RS485 variant overlay (alias `hcp-rs485-dir`) is present.
 * Backend: zephyr (uart_api) on target, in-memory loopback on host.
 * Google C++ Style naming.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace hcp {
namespace serial {

class Port {
 public:
  Port();
  ~Port();
  Port(const Port&) = delete;
  Port& operator=(const Port&) = delete;

  bool Open(uint32_t baud);
  void Close();
  bool IsOpen() const;

  int Write(const uint8_t* data, size_t len);  // bytes written, or -1
  int Read(uint8_t* buf, size_t cap, uint32_t timeout_ms);  // bytes read

 private:
  struct Impl;
  Impl* impl_;
};

}  // namespace serial
}  // namespace hcp
