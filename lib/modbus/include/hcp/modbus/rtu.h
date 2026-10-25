/*
 * hcp::modbus - RTU frame engine, independent of any UART.
 *
 * Zephyr's in-tree subsys/modbus ends a received frame on line idle (t3.5), which does not
 * work against the meter on the bench: its slave takes 210-480 ms before its *first* byte,
 * and the subsys' rx_fifo_drain() is only called from an RX interrupt that is disabled at
 * that point - dead code. The result was a reply that is provably correct on the wire (a
 * raw probe reads 29 bytes with a matching CRC every time) being rejected -EIO on every
 * poll.
 *
 * So this frames a transaction the way the previous firmware's master did
 * (managed_components/espressif__esp-modbus/freemodbus/port/portserial.c): flush the
 * receiver, write the request, then read **the byte count the function code implies**
 * under one deadline, suppressing the echo a half-duplex line hands back. No frame
 * boundary is ever guessed from timing.
 *
 * Bytes in, bytes out through a two-method interface, so the slow-reply, echo, exception
 * and truncated-frame cases are host-tested with a scripted transport instead of
 * discovered on the bench once a week.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace hcp {
namespace modbus {

/* The two calls the frame engine needs. Blocking-with-timeout semantics, matching
 * hcp::serial::Port, so the real transport is a five-line adapter and the tests can
 * hand over a scripted byte sequence. */
struct ByteIo {
  virtual ~ByteIo() = default;

  // Sends the whole buffer (the implementation may block until it is on the wire).
  virtual void Write(const uint8_t* data, size_t len) = 0;

  // Up to `cap` bytes into `buf`, waiting no longer than `timeout_ms`. Returns the
  // number actually read, which may be 0 (timeout) or less than `cap`.
  virtual int Read(uint8_t* buf, size_t cap, uint32_t timeout_ms) = 0;

  // Monotonic milliseconds. The engine bounds a reply by one overall deadline
  // rather than by a gap between bytes, so it needs a clock; taking it from the io
  // object is what lets the tests run a 480 ms slave without sleeping 480 ms.
  virtual uint64_t NowMs() = 0;
};

// Function codes used by the collector and by Home Assistant's plugins.
constexpr uint8_t kFcReadCoils = 0x01;
constexpr uint8_t kFcReadDinputs = 0x02;
constexpr uint8_t kFcReadHolding = 0x03;
constexpr uint8_t kFcReadInput = 0x04;
constexpr uint8_t kFcWriteCoil = 0x05;
constexpr uint8_t kFcWriteSingle = 0x06;
constexpr uint8_t kFcWriteMultiple = 0x10;

constexpr int kMaxRegsPerReply = 123;  // 256-byte ADU: 6 header + 2 CRC + 2*n
constexpr int kRawLogLen = 32;        // what we keep of a frame for diagnosis

struct Request {
  uint8_t unit = 1;
  uint8_t fc = kFcReadHolding;
  uint16_t start = 0;
  uint16_t count = 1;         // registers, or bits; 1 for the single writes
  const uint16_t* values = nullptr;  // for kFcWriteMultiple / kFcWriteSingle
  bool coil_on = false;              // for kFcWriteCoil
};

/* What a reply turned out to be. kOk means the frame is complete, its CRC checks
 * and it came from the unit we asked - the only case where the caller may use the
 * payload. Every failure keeps raw_len/raw so the log can say what actually
 * arrived: that visibility is the reason this engine exists. */
enum class Frame : uint8_t {
  kOk = 0,
  kException,     // the slave answered with fc|0x80; exception_code is set
  kBadCrc,        // right length, CRC does not match
  kWrongUnit,     // another slave answered, or our own echo survived suppression
  kTruncated,     // deadline passed with fewer bytes than the frame needs
  kNoReply,       // nothing at all arrived
  kTooBig,        // reply claims more registers than we can hold
  kTimeoutBogus,  // first byte arrived, but not a reply we can size
};

struct Reply {
  uint8_t unit = 0;
  uint8_t fc = 0;
  uint8_t exception_code = 0;              // valid for kException
  uint16_t reg_count = 0;                  // entries in regs[]
  uint16_t regs[kMaxRegsPerReply] = {};    // host order
  uint8_t bits[(kMaxRegsPerReply * 16 + 7) / 8] = {};  // for kFcReadCoils/dinputs
  size_t raw_len = 0;
  uint8_t raw[kRawLogLen] = {};
};

// Modbus CRC-16 (poly 0xA001, init 0xFFFF), transmitted low byte first.
uint16_t Crc16(const uint8_t* data, size_t len);

// Build a request ADU including its CRC. Returns its length, or 0 when the request
// is out of range (count, values) or does not fit in `out`.
size_t BuildRequest(const Request& req, uint8_t* out, size_t cap);

// How many bytes a valid reply to `fc` must have once the byte count is known, and
// how many we must read before that count is even visible (3). Returns 0 when the
// function code is not one this engine frames.
size_t ExpectedReplyLen(uint8_t fc, uint8_t byte_count_field);

/*
 * One transaction: flush, write, read the sized frame, verify.
 *
 * `timeout_ms` is the overall budget for the reply (the meter on the bench uses
 * ~210-480 ms of it), not a per-byte gap. Echo suppression: bytes that arrive
 * immediately after the write and match the request we just sent are discarded -
 * that is a half-duplex line handing us our own signal, and it is what shifts a
 * frame by N bytes and makes a good reply look like a CRC error.
 *
 * Returns Frame; on kOk the payload in `out` is usable.
 */
Frame Transact(ByteIo& io, const Request& req, uint32_t timeout_ms, Reply* out);

}  // namespace modbus
}  // namespace hcp
