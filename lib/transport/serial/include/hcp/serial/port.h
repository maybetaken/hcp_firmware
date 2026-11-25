/*
 * HCP serial transport — the Modbus UART (devicetree alias `hcp-uart`).
 * A thin byte-pipe over the target UART; RS485 direction (DE) is layered on
 * when the RS485 variant overlay (alias `hcp-rs485-dir`) is present.
 * Backend: zephyr (uart_api) on target, in-memory loopback on host.
 * Google C++ Style naming.
 *
 * Copyright (c) 2026 maybetaken
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
  /* Line framing, separate from Open() because one wire can carry more than one
   * protocol: the reference iotrix module sends DL/T645 at 2400 8E1 and Modbus at
   * 9600 8N1 over the same RS485 pair, choosing per frame. Plain types, no Zephyr
   * header, so the host backend sees the same contract. */
  enum class Parity : uint8_t { kNone = 0, kEven, kOdd };

  struct Line {
    uint32_t baud = 9600;
    Parity parity = Parity::kNone;
    uint8_t stop_bits = 1;  // 1 or 2
  };

  Port();
  ~Port();
  Port(const Port&) = delete;
  Port& operator=(const Port&) = delete;

  bool Open(uint32_t baud);  // 8N1 at `baud`
  void Close();
  bool IsOpen() const;

  /* Reconfigure the live port. False (old framing left in place) when the port is
   * not open or the driver rejects the parameters. */
  bool SetLine(const Line& line);
  Line line() const;

  int Write(const uint8_t* data, size_t len);  // bytes written, or -1
  int Read(uint8_t* buf, size_t cap, uint32_t timeout_ms);  // bytes read

 private:
  struct Impl;
  Impl* impl_;
};

}  // namespace serial
}  // namespace hcp
