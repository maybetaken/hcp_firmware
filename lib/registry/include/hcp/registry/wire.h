/*
 * hcp::registry - the byte layout of the two topics that carry binary, `notify`
 * (device -> Home Assistant) and `control/cmd` (Home Assistant -> device).
 *
 * This is the contract with custom_components/solar_manager/protocol_helper/
 * modbus_protocol_helper.py, which decodes with:
 *
 *     struct.unpack(">BBHH", payload[:6])          # unit, fc, start, count
 *     struct.unpack(f"{endian_prefix}{fmt}", ...)   # the data after those 6 bytes
 *
 * so the header is big-endian and the data words are big-endian too. The old
 * firmware produced that by byte-swapping each host-order uint16_t after the read
 * (main/solar_manager.cpp pollingTaskLoop), and Zephyr's subsys hands us host-order
 * registers the same way - so the swap has to be carried over, not "cleaned up".
 *
 * Getting this wrong is silent: the device still polls, Home Assistant still
 * decodes, and every value is garbage. Which is why the vectors below are taken
 * from the real payloads rather than invented, and why the packing lives here as
 * pure functions with host tests instead of inline in the collector's loop.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "hcp/registry/table.h"

namespace hcp {
namespace registry {

// Bytes the `notify` payload uses for [unit, fc, start, count] before the data.
constexpr int kNotifyHeaderBytes = 6;

/*
 * Build a `notify` payload for a register read (FC03/FC04): the 6-byte header,
 * then each register as 2 bytes big-endian, in order.
 *
 * Returns the length written, or 0 if it would not fit in `out`/`cap` or n does
 * not match seg.length (a mismatch means the caller and the table disagree about
 * what was read, which is a bug worth stopping rather than reporting).
 */
size_t PackRegisters(const Segment& seg, const uint16_t* regs, uint16_t n,
                     uint8_t* out, size_t cap);

/*
 * Build a `notify` payload for a bit read (FC01/FC02): header, then one byte per
 * 8 bits, MSB first, matching what the Modbus response itself carries.
 */
size_t PackBits(const Segment& seg, const uint8_t* bits, uint16_t count,
                 uint8_t* out, size_t cap);

/*
 * One write request out of a `control/cmd` payload: the same 6-byte header the
 * device sends on `notify`, followed by the values, i.e. what HA's
 * modbus_protocol_helper packs for a write.
 *
 * Fixed inline storage, not a pointer into the MQTT buffer: the caller frees that
 * buffer as soon as it has decoded, and the previous firmware's habit of passing
 * malloc'd pointers onwards through a queue is what this replaces.
 */
constexpr int kMaxWriteValues = 16;

struct WriteRequest {
  uint8_t unit_id;
  uint8_t function;                  // 0x05, 0x06 or 0x10
  uint16_t start_address;
  uint16_t count = 0;                // registers for 0x10, else 1
  uint16_t values[kMaxWriteValues] = {};  // host order, count entries
  bool coil_on = false;              // FC05 only
};

enum class WriteParse : uint8_t {
  kBad = 0,      // short, unknown function code, or count/value mismatch
  kOk,
};

/*
 * Read a write request from a `control/cmd` payload. Length-bounded (the buffer is
 * the MQTT payload, not NUL-terminated), so nothing here trusts the header.
 *
 * Accepted shapes, as the previous firmware emitted them:
 *   [unit, 0x06, addr_hi, addr_lo, v_hi, v_lo]                 one register
 *   [unit, 0x05, addr_hi, addr_lo, 0x00/0xFF, 0x00/0xFF]       one coil
 *   [unit, 0x10, addr_hi, addr_lo, n_hi, n_lo, v0_hi, v0_lo…]  several registers
 */
WriteParse ParseWriteRequest(const uint8_t* buf, size_t len, WriteRequest* out);

}  // namespace registry
}  // namespace hcp
