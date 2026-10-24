/*
 * hcp::modbus - see rtu.h for why this frames a reply by length rather than by an
 * idle line, and which behaviour in the previous firmware it is modelled on.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "hcp/modbus/rtu.h"

#include <string.h>

namespace hcp {
namespace modbus {
namespace {

constexpr int kMaxFrame = 256;

void Remember(Reply* out, const uint8_t* buf, size_t len) {
  out->raw_len = len;
  const size_t n = len < static_cast<size_t>(kRawLogLen) ? len : static_cast<size_t>(kRawLogLen);
  memcpy(out->raw, buf, n);
}

bool IsReadFc(uint8_t fc) {
  return fc == kFcReadCoils || fc == kFcReadDinputs || fc == kFcReadHolding ||
         fc == kFcReadInput;
}

bool IsBitFc(uint8_t fc) { return fc == kFcReadCoils || fc == kFcReadDinputs; }

}  // namespace

uint16_t Crc16(const uint8_t* data, size_t len) {
  uint16_t c = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    c ^= data[i];
    for (int b = 0; b < 8; b++) {
      c = static_cast<uint16_t>((c & 1U) ? ((c >> 1) ^ 0xA001U) : (c >> 1));
    }
  }
  return c;
}

size_t BuildRequest(const Request& req, uint8_t* out, size_t cap) {
  if (out == nullptr || req.unit == 0 || req.unit > 247) {
    return 0;
  }

  size_t n = 0;
  out[n++] = req.unit;
  out[n++] = req.fc;
  out[n++] = static_cast<uint8_t>(req.start >> 8);
  out[n++] = static_cast<uint8_t>(req.start & 0xFF);

  if (IsReadFc(req.fc)) {
    const uint16_t limit =
        IsBitFc(req.fc) ? static_cast<uint16_t>(sizeof(Reply::bits) * 8)
                        : static_cast<uint16_t>(kMaxRegsPerReply);
    if (req.count == 0 || req.count > limit) {
      return 0;
    }
    out[n++] = static_cast<uint8_t>(req.count >> 8);
    out[n++] = static_cast<uint8_t>(req.count & 0xFF);
  } else if (req.fc == kFcWriteCoil) {
    out[n++] = 0;  // a coil is 0x0000 (off) or 0xFF00 (on), nothing else
    out[n++] = req.coil_on ? 0xFF : 0x00;
  } else if (req.fc == kFcWriteSingle) {
    if (req.values == nullptr) {
      return 0;
    }
    out[n++] = static_cast<uint8_t>(req.values[0] >> 8);
    out[n++] = static_cast<uint8_t>(req.values[0] & 0xFF);
  } else if (req.fc == kFcWriteMultiple) {
    if (req.values == nullptr || req.count < 1 || req.count > kMaxRegsPerReply) {
      return 0;
    }
    out[n++] = static_cast<uint8_t>(req.count >> 8);
    out[n++] = static_cast<uint8_t>(req.count & 0xFF);
    out[n++] = static_cast<uint8_t>(req.count * 2);
    for (uint16_t i = 0; i < req.count; i++) {
      out[n++] = static_cast<uint8_t>(req.values[i] >> 8);
      out[n++] = static_cast<uint8_t>(req.values[i] & 0xFF);
    }
  } else {
    return 0;  // a function code this master does not send
  }

  const uint16_t crc = Crc16(out, n);
  out[n++] = static_cast<uint8_t>(crc & 0xFF);  // low byte first, on the wire
  out[n++] = static_cast<uint8_t>(crc >> 8);
  return n <= cap ? n : 0;
}

size_t ExpectedReplyLen(uint8_t fc, uint8_t byte_count_field) {
  if (fc & 0x80) {
    return 5;  // unit, fc|0x80, exception code, crc*2
  }
  switch (fc) {
    case kFcReadCoils:
    case kFcReadDinputs:
    case kFcReadHolding:
    case kFcReadInput:
      return 3U + byte_count_field + 2U;
    case kFcWriteCoil:
    case kFcWriteSingle:
    case 0x0F:
    case kFcWriteMultiple:
      return 8;  // echo: unit, fc, addr*2, value/qty*2, crc*2
    default:
      return 0;
  }
}

Frame Transact(ByteIo& io, const Request& req, uint32_t timeout_ms, Reply* out) {
  uint8_t frame[kMaxFrame];
  const size_t reqlen = BuildRequest(req, frame, sizeof(frame));
  if (reqlen == 0) {
    return Frame::kTooBig;
  }

  /* Drop anything already sitting in the receiver. A reply to a request we gave up
   * on is exactly the thing that shifts the next frame by a few bytes and then
   * fails its CRC while being perfectly good on the wire. */
  uint8_t junk[64];
  int stale = 0;
  while (io.Read(junk, sizeof(junk), 0) > 0) {
    stale += 64;
  }

  io.Write(frame, reqlen);
  const uint64_t deadline = io.NowMs() + timeout_ms;

  /* Echo suppression. hcp::serial::Port's Write returns once the shift register is
   * empty (it holds DE high until then), so on a line that hands our own request
   * back, those bytes are already in the receiver here and are read without
   * waiting. They are discarded only if they are literally the bytes we sent. */
  const bool may_echo = IsReadFc(req.fc);
  uint8_t buf[kMaxFrame];
  size_t got = 0;
  if (may_echo) {
    while (got < reqlen) {
      const int n = io.Read(buf + got, sizeof(buf) - got, 0);
      if (n <= 0 || static_cast<size_t>(n) > sizeof(buf) - got) {
        break;
      }
      got += static_cast<size_t>(n);
    }
    bool echo = got > 0 && got <= reqlen;
    for (size_t i = 0; echo && i < got; i++) {
      if (buf[i] != frame[i]) {
        echo = false;
      }
    }
    if (echo) {
      stale += static_cast<int>(got);
      got = 0;  // our own request; the slave's answer is still to come
    }
  }

  /* Two bytes identify the reply: the unit we asked and the function code (or that
   * code with the exception bit set). Nothing after this is sized by what the
   * reply claims - see below. */
  while (got < 2) {
    const int64_t left = static_cast<int64_t>(deadline - io.NowMs());
    if (left <= 0) {
      Remember(out, buf, got);
      return got == 0 ? Frame::kNoReply : Frame::kTruncated;
    }
    const int n = io.Read(buf + got, sizeof(buf) - got, static_cast<uint32_t>(left));
    if (n > 0) {
      got += static_cast<size_t>(n);
    }
  }

  if (buf[0] != req.unit) {
    Remember(out, buf, got);
    out->unit = buf[0];
    out->fc = buf[1];
    return Frame::kWrongUnit;
  }

  const bool exception = (buf[1] & 0x80) != 0;
  if (!exception && buf[1] != req.fc) {
    Remember(out, buf, got);
    out->unit = buf[0];
    out->fc = buf[1];
    return Frame::kTimeoutBogus;  // somebody else's frame; the log shows the bytes
  }

  /* The length is derived from the request: an exception is 5 bytes, a write is an
   * 8-byte echo, a read is 5 + the payload we asked for. Trusting the byte-count
   * field instead is what lets a shifted or partial frame look well-formed. */
  size_t want;
  if (exception) {
    want = 5;
  } else if (IsReadFc(req.fc)) {
    want = 5 + (IsBitFc(req.fc) ? (static_cast<size_t>(req.count) + 7) / 8
                                : static_cast<size_t>(req.count) * 2);
  } else {
    want = 8;
  }
  if (want > sizeof(buf)) {
    Remember(out, buf, got);
    return Frame::kTooBig;
  }

  while (got < want) {
    const int64_t left = static_cast<int64_t>(deadline - io.NowMs());
    if (left <= 0) {
      Remember(out, buf, got);
      out->unit = buf[0];
      out->fc = buf[1];
      return Frame::kTruncated;
    }
    const size_t room = sizeof(buf) - got < want - got ? sizeof(buf) - got : want - got;
    const int n = io.Read(buf + got, room, static_cast<uint32_t>(left));
    if (n > 0) {
      got += static_cast<size_t>(n);
    }
  }

  Remember(out, buf, got);
  out->unit = buf[0];
  out->fc = buf[1];

  const uint16_t on_wire = static_cast<uint16_t>(buf[got - 2] | (buf[got - 1] << 8));
  if (Crc16(buf, got - 2) != on_wire) {
    return Frame::kBadCrc;
  }

  if (exception) {
    out->exception_code = buf[2];
    return Frame::kException;
  }

  if (IsReadFc(buf[1])) {
    const size_t payload = got - 5;  // everything between the count byte and the CRC
    if (buf[2] != payload) {
      return Frame::kTimeoutBogus;  // count field disagrees with the frame we sized
    }
    if (IsBitFc(buf[1])) {
      memcpy(out->bits, &buf[3], payload);
      out->reg_count = req.count;  // bits, not bytes
    } else {
      out->reg_count = static_cast<uint16_t>(payload / 2);
      for (uint16_t i = 0; i < out->reg_count; i++) {
        // the wire is big-endian per register; keep host order in the struct
        out->regs[i] = static_cast<uint16_t>((buf[3 + 2 * i] << 8) | buf[4 + 2 * i]);
      }
    }
  }
  return Frame::kOk;
}

}  // namespace modbus
}  // namespace hcp
