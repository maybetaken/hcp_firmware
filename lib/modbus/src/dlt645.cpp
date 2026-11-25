/*
 * hcp::modbus - DL/T645-2007 framing. See dlt645.h for why this sits beside the
 * Modbus engine instead of in a protocol of its own.
 *
 * Copyright (c) 2026 maybetaken
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "hcp/modbus/dlt645.h"

#include <string.h>

namespace hcp {
namespace modbus {
namespace {

constexpr size_t kMaxFrame = 256;
/* 68 + addr(6) + 68 + ctrl + len, then the data, then cs + 16. */
constexpr size_t kHeaderLen = 10;
constexpr size_t kTailLen = 2;

/* Leading 0xFE bytes, at most 4. Returns the count, or kNoPreamble when the first
 * byte is neither 0xFE nor 0x68 - i.e. this is not a 645 frame at all. */
constexpr size_t kNoPreamble = static_cast<size_t>(-1);

size_t Preamble(const uint8_t* buf, size_t len) {
  size_t p = 0;
  while (p < len && p < 4 && buf[p] == 0xFE) {
    p++;
  }
  if (p == len || buf[p] != 0x68) {
    return kNoPreamble;
  }
  return p;
}

/* Sum from the first 0x68 through the last data byte, mod 256. */
uint8_t Checksum(const uint8_t* buf, size_t from, size_t to) {
  uint8_t sum = 0;
  for (size_t i = from; i < to; i++) {
    sum = static_cast<uint8_t>(sum + buf[i]);
  }
  return sum;
}

void Remember(Reply645* out, const uint8_t* buf, size_t len) {
  out->raw_len = len;
  const size_t n = len < static_cast<size_t>(kRawLogLen) ? len
                                                        : static_cast<size_t>(kRawLogLen);
  memcpy(out->raw, buf, n);
}

}  // namespace

size_t Dlt645FrameLen(const uint8_t* buf, size_t len) {
  const size_t p = Preamble(buf, len);
  if (p == kNoPreamble || len < p + kHeaderLen) {
    return 0;
  }
  if (buf[p + 7] != 0x68) {
    return 0;
  }
  return p + kHeaderLen + buf[p + 9] + kTailLen;
}

bool IsDlt645(const uint8_t* buf, size_t len) {
  if (buf == nullptr || len == 0 || buf[0] != 0xFE) {
    return false;
  }
  const size_t want = Dlt645FrameLen(buf, len);
  if (want != len || buf[len - 1] != 0x16) {
    return false;
  }
  const size_t p = Preamble(buf, len);
  return Checksum(buf, p, len - 2) == buf[len - 2];
}

Frame645 Transact645(ByteIo& io, const uint8_t* frame, size_t len,
                     uint32_t timeout_ms, Reply645* out) {
  out->raw_len = 0;
  out->ctrl = 0;
  if (!IsDlt645(frame, len) || len > kMaxFrame) {
    return Frame645::kBadFrame;
  }

  /* A reply to a request we gave up on shifts the next frame by a few bytes and
   * then fails its checksum while being perfectly good on the wire. */
  uint8_t junk[64];
  while (io.Read(junk, sizeof(junk), 0) > 0) {
  }

  io.Write(frame, len);
  const uint64_t deadline = io.NowMs() + timeout_ms;

  /* Half-duplex echo: Port::Write returns with the shift register drained, so our
   * own frame is already in the receiver here and reads without waiting. Discard
   * it only if it is literally what we sent. */
  uint8_t buf[kMaxFrame];
  size_t got = 0;
  while (got < len) {
    const int n = io.Read(buf + got, sizeof(buf) - got, 0);
    if (n <= 0) {
      break;
    }
    got += static_cast<size_t>(n);
  }
  bool echo = got > 0 && got <= len;
  for (size_t i = 0; echo && i < got; i++) {
    if (buf[i] != frame[i]) {
      echo = false;
    }
  }
  if (echo) {
    got = 0;
  }

  size_t want = 0;
  while ((want = Dlt645FrameLen(buf, got)) == 0) {
    if (got > 0 && buf[0] != 0xFE && buf[0] != 0x68) {
      Remember(out, buf, got);
      return Frame645::kBadFrame;  // somebody else's traffic; the log shows it
    }
    const size_t p = Preamble(buf, got);
    if (p != kNoPreamble && got >= p + kHeaderLen && buf[p + 7] != 0x68) {
      Remember(out, buf, got);
      return Frame645::kBadFrame;  // second start marker missing: not a 645 frame
    }
    if (got >= sizeof(buf)) {
      Remember(out, buf, got);
      return Frame645::kBadFrame;
    }
    const int64_t left = static_cast<int64_t>(deadline - io.NowMs());
    if (left <= 0) {
      Remember(out, buf, got);
      return got == 0 ? Frame645::kNoReply : Frame645::kTruncated;
    }
    const int n = io.Read(buf + got, sizeof(buf) - got, static_cast<uint32_t>(left));
    if (n > 0) {
      got += static_cast<size_t>(n);
    }
  }
  if (want > sizeof(buf)) {
    Remember(out, buf, got);
    return Frame645::kBadFrame;
  }

  while (got < want) {
    const int64_t left = static_cast<int64_t>(deadline - io.NowMs());
    if (left <= 0) {
      Remember(out, buf, got);
      return Frame645::kTruncated;
    }
    const size_t room = sizeof(buf) - got < want - got ? sizeof(buf) - got : want - got;
    const int n = io.Read(buf + got, room, static_cast<uint32_t>(left));
    if (n > 0) {
      got += static_cast<size_t>(n);
    }
  }

  Remember(out, buf, got);
  const size_t p = Preamble(buf, got);
  if (buf[got - 1] != 0x16 || Checksum(buf, p, got - 2) != buf[got - 2]) {
    return Frame645::kBadCs;
  }
  /* The request's address, not the preamble count: a slave may answer with a
   * different number of leading 0xFE than we sent. */
  const size_t req_p = Preamble(frame, len);
  if (memcmp(buf + p + 1, frame + req_p + 1, 6) != 0) {
    return Frame645::kWrongAddr;
  }
  out->ctrl = buf[p + 8];
  return (out->ctrl & 0x40) != 0 ? Frame645::kError : Frame645::kOk;
}

}  // namespace modbus
}  // namespace hcp