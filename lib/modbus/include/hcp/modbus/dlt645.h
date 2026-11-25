/*
 * hcp::modbus - DL/T645-2007 on the same wire as Modbus.
 *
 * The ddsu666 meter speaks two protocols on one RS485 pair: Modbus-RTU for its
 * measurements, DL/T645-2007 for the command that switches it between them. The
 * reference iotrix module (mppt-v1-cpp-26.11.19, ddsu666_write @0x400E60E4) does not
 * have a "switch the port" command at all - it looks at the outgoing frame, and if
 * it carries the 645 preamble it sends it at 2400 8E1, otherwise at 9600 with no
 * parity. The frame itself goes out verbatim either way; the module never inspects
 * a register address.
 *
 * So this lives beside rtu.h rather than in a protocol of its own: it is the other
 * framing of the same bus, over the same ByteIo, sized by its own length field for
 * the same reason (see rtu.h - an idle line cannot frame this meter's reply).
 *
 * Copyright (c) 2026 maybetaken
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "hcp/modbus/rtu.h"

namespace hcp {
namespace modbus {

/* What the reference module uses for a 645 frame, and what DDSU666.pdf p9 lists as
 * the factory DL/T645 settings. Parity even, one stop bit. */
constexpr uint32_t kDlt645Baud = 2400;

/*
 * True when `buf` is a whole DL/T645-2007 frame rather than a Modbus RTU one:
 * 1..4 leading 0xFE, then 68 addr(6) 68 ctrl len data[len] cs 16, with the checksum
 * matching and no trailing bytes.
 *
 * The preamble is what separates the two on `control/cmd`: 0xFE is 254, which is
 * not a legal Modbus unit address, so a Modbus frame can never start with it.
 */
bool IsDlt645(const uint8_t* buf, size_t len);

/*
 * Total frame length implied by the header, or 0 when `len` bytes are not yet
 * enough to know. Unlike IsDlt645 this tolerates a missing preamble, because a
 * slave is not obliged to send one.
 */
size_t Dlt645FrameLen(const uint8_t* buf, size_t len);

enum class Frame645 : uint8_t {
  kOk = 0,
  kError,      // well-formed reply, but ctrl bit6 says the slave refused
  kBadCs,      // right length, checksum does not match
  kWrongAddr,  // another meter answered
  kTruncated,  // deadline passed with fewer bytes than the frame needs
  kNoReply,    // nothing at all arrived
  kBadFrame,   // not a frame we can size, or too long for the buffer
};

struct Reply645 {
  uint8_t ctrl = 0;
  size_t raw_len = 0;
  uint8_t raw[kRawLogLen] = {};
};

/*
 * One 645 transaction: flush, write the frame verbatim, read the sized reply,
 * verify. `frame` must pass IsDlt645 - the caller has already classified it, and
 * re-checking here is what keeps a malformed payload off the wire.
 *
 * Same overall-deadline and echo-suppression behaviour as Transact (see rtu.h).
 * The caller owns the line settings: this only moves bytes.
 */
Frame645 Transact645(ByteIo& io, const uint8_t* frame, size_t len,
                     uint32_t timeout_ms, Reply645* out);

}  // namespace modbus
}  // namespace hcp