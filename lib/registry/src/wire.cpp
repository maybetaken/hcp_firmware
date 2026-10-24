/*
 * hcp::registry - see wire.h for what the byte layout is and why the big-endian
 * swap has to stay.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "hcp/registry/wire.h"

#include <ctype.h>
#include <string.h>

namespace hcp {
namespace registry {
namespace {

void PutHeader(uint8_t* out, const Segment& seg) {
  out[0] = seg.slave_id;
  out[1] = seg.read_command;
  out[2] = (uint8_t)(seg.start_address >> 8);
  out[3] = (uint8_t)(seg.start_address & 0xFF);
  out[4] = (uint8_t)(seg.length >> 8);
  out[5] = (uint8_t)(seg.length & 0xFF);
}

uint16_t Be16(const uint8_t* p) {
  return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

}  // namespace

size_t PackRegisters(const Segment& seg, const uint16_t* regs, uint16_t n,
                     uint8_t* out, size_t cap) {
  if (regs == nullptr || out == nullptr || n != seg.length) {
    return 0;
  }
  const size_t need = (size_t)kNotifyHeaderBytes + (size_t)n * 2u;
  if (need > cap) {
    return 0;
  }
  PutHeader(out, seg);
  for (uint16_t i = 0; i < n; i++) {
    // the swap the old firmware did, and Home Assistant still decodes: host-order
    // uint16_t in, big-endian bytes out
    out[kNotifyHeaderBytes + 2 * i] = (uint8_t)(regs[i] >> 8);
    out[kNotifyHeaderBytes + 2 * i + 1] = (uint8_t)(regs[i] & 0xFF);
  }
  return need;
}

size_t PackBits(const Segment& seg, const uint8_t* bits, uint16_t count,
                 uint8_t* out, size_t cap) {
  if (bits == nullptr || out == nullptr || count != seg.length) {
    return 0;
  }
  const size_t data = ((size_t)count + 7u) / 8u;
  const size_t need = (size_t)kNotifyHeaderBytes + data;
  if (need > cap) {
    return 0;
  }
  PutHeader(out, seg);
  memcpy(out + kNotifyHeaderBytes, bits, data);
  return need;
}

WriteParse ParseWriteRequest(const uint8_t* buf, size_t len, WriteRequest* out) {
  if (buf == nullptr || out == nullptr || len < (size_t)kNotifyHeaderBytes) {
    return WriteParse::kBad;
  }

  const uint8_t unit = buf[0];
  const uint8_t fc = buf[1];
  // unit 0 is the broadcast address: nothing can answer, and writing blind to
  // every slave on the bus is not something an MQTT command should be able to do
  if (unit == 0 || unit > 247) {
    return WriteParse::kBad;
  }

  WriteRequest req{};
  req.unit_id = unit;
  req.function = fc;
  req.start_address = Be16(buf + 2);

  switch (fc) {
    case 0x05:  // write coil
    case 0x06: {                        // write single register
      if (len != (size_t)kNotifyHeaderBytes) {
        return WriteParse::kBad;
      }
      const uint16_t v = Be16(buf + 4);
      req.count = 1;
      req.values[0] = v;
      if (fc == 0x05) {
        // Modbus allows only 0xFF00/0x0000 for a coil
        if (v != 0xFF00 && v != 0x0000) {
          return WriteParse::kBad;
        }
        req.coil_on = (v == 0xFF00);
      }
      *out = req;
      return WriteParse::kOk;
    }

    case 0x10: {                        // write multiple registers
      const uint16_t n = Be16(buf + 4);
      if (n == 0 || n > kMaxWriteValues) {
        return WriteParse::kBad;  // refused, never truncated
      }
      if (len != (size_t)kNotifyHeaderBytes + (size_t)n * 2u) {
        return WriteParse::kBad;  // header does not match the bytes present
      }
      req.count = n;
      for (uint16_t i = 0; i < n; i++) {
        req.values[i] = Be16(buf + kNotifyHeaderBytes + 2 * i);
      }
      *out = req;
      return WriteParse::kOk;
    }

    default:
      return WriteParse::kBad;
  }
}

LedSetting ParseLedCommand(const uint8_t* buf, size_t len) {
  if (buf == nullptr) {
    return LedSetting::kInvalid;
  }
  // Case-insensitive and tolerant of a trailing newline, because the payload is
  // written by hand in YAML automations as often as it is by the integration.
  auto Is = [&](const char* word) {
    const size_t n = strlen(word);
    if (len != n && len != n + 1) {
      return false;
    }
    for (size_t i = 0; i < n; i++) {
      char a = static_cast<char>(tolower(buf[i]));
      if (a != word[i]) {
        return false;
      }
    }
    return len == n || buf[n] == '\n' || buf[n] == '\r';
  };
  if (Is("on")) {
    return LedSetting::kOn;
  }
  if (Is("off")) {
    return LedSetting::kOff;
  }
  return LedSetting::kInvalid;
}

}  // namespace registry
}  // namespace hcp
