/*
 * hcp::modbus::Master - the native RTU backend.
 *
 * Same public API as the subsys backend (see master.h): the collector does not know
 * which one it is talking to. Underneath it is hcp::serial::Port (which also drives
 * the RS485 DE pin when the board has one) plus the frame engine in rtu.cpp.
 *
 * Why a second backend exists at all is written up in rtu.h: the in-tree subsys
 * frames a reply by line idle and cannot see the byte-level activity that a slow
 * meter produces, so on the bench C3 every poll failed CRC while a raw probe of the
 * same wire read a perfect frame every time. c5 and c6 keep the subsys.
 *
 * Copyright (c) 2026 maybetaken
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>

#include "hcp/log/log.h"
#include "hcp/modbus/dlt645.h"
#include "hcp/modbus/master.h"
#include "hcp/modbus/rtu.h"
#include "hcp/serial/port.h"

namespace hcp {
namespace modbus {
namespace {

// What a frame outcome means as an errno, so callers see the same vocabulary the
// subsys backend used (-ENOTCONN "nobody answered", -EIO "malformed ADU") and the
// collector's logging did not have to change.
int ErrnoFor(Frame f) {
  switch (f) {
    case Frame::kOk:
      return 0;
    case Frame::kNoReply:
      return -ENOTCONN;
    case Frame::kTruncated:
      return -ETIMEDOUT;
    case Frame::kBadCrc:
      return -EIO;
    case Frame::kWrongUnit:
      return -EADDRNOTAVAIL;
    case Frame::kException:
      return -EPROTO;
    case Frame::kTooBig:
      return -EFBIG;
    default:
      return -EMSGSIZE;
  }
}

const char* Name(Frame f) {
  switch (f) {
    case Frame::kOk: return "ok";
    case Frame::kNoReply: return "no reply";
    case Frame::kTruncated: return "truncated";
    case Frame::kBadCrc: return "crc";
    case Frame::kWrongUnit: return "wrong unit";
    case Frame::kException: return "exception";
    case Frame::kTooBig: return "too big";
    default: return "unframable";
  }
}

// Up to 12 bytes of what actually arrived. Seeing the bytes is the point of this
// backend: the failure it replaces gave nothing but an error code.
void Describe(const uint8_t* raw, size_t raw_len, char* out, size_t cap) {
  size_t used = 0;
  out[0] = '\0';
  const size_t n = raw_len < 12 ? raw_len : 12;
  for (size_t i = 0; i < n && used + 3 < cap; i++) {
    used += static_cast<size_t>(
        snprintk(out + used, cap - used, "%s%02X", i ? " " : "", raw[i]));
  }
  if (raw_len > n && used + 4 < cap) {
    used += static_cast<size_t>(snprintk(out + used, cap - used, " .."));
  }
}

const char* Name645(Frame645 f) {
  switch (f) {
    case Frame645::kOk: return "ok";
    case Frame645::kError: return "refused";
    case Frame645::kBadCs: return "checksum";
    case Frame645::kWrongAddr: return "wrong addr";
    case Frame645::kTruncated: return "truncated";
    case Frame645::kNoReply: return "no reply";
    default: return "unframable";
  }
}

int Errno645(Frame645 f) {
  switch (f) {
    case Frame645::kOk: return 0;
    case Frame645::kError: return -EPROTO;
    case Frame645::kBadCs: return -EIO;
    case Frame645::kWrongAddr: return -EADDRNOTAVAIL;
    case Frame645::kTruncated: return -ETIMEDOUT;
    case Frame645::kNoReply: return -ENOTCONN;
    default: return -EMSGSIZE;
  }
}

}  // namespace

struct Master::Impl {
  serial::Port port;

  /* The frame engine's two calls, onto the Port. NowMs() is the uptime clock: the
   * engine bounds a reply by one overall deadline instead of by a gap. */
  class Io : public ByteIo {
   public:
    explicit Io(serial::Port* p) : p_(p) {}
    void Write(const uint8_t* data, size_t len) override { p_->Write(data, len); }
    int Read(uint8_t* buf, size_t cap, uint32_t timeout_ms) override {
      return p_->Read(buf, cap, timeout_ms);
    }
    uint64_t NowMs() override { return k_uptime_get(); }

   private:
    serial::Port* p_;
  } io{&port};

  Config cfg;
  bool open = false;
};

Master::Master() : impl_(new Impl()) {}
Master::~Master() { delete impl_; }

bool Master::Open(const Config& cfg) {
  impl_->cfg = cfg;
  if (!impl_->port.Open(cfg.baud)) {
    last_error_ = -EIO;
    HCP_LOGE(hcp::log::kDomainModbus,
             "native RTU master: uart unavailable (no hcp-uart alias?)");
    return false;
  }
  impl_->open = true;
  last_error_ = 0;
  HCP_LOGI(hcp::log::kDomainModbus,
           "native RTU master up: baud %u, response timeout %u ms",
           (unsigned)cfg.baud, (unsigned)cfg.response_timeout_ms);
  return true;
}

void Master::Close() {
  if (impl_->open) {
    impl_->port.Close();
    impl_->open = false;
  }
}

bool Master::Iface() { return impl_->open; }

bool Master::Transceive(const Request& req, Reply* rep, const char* what,
                        uint16_t count) {
  if (!impl_->open) {
    last_error_ = -1;
    return false;
  }
  const Frame f = Transact(impl_->io, req, impl_->cfg.response_timeout_ms, rep);
  last_error_ = ErrnoFor(f);
  if (f == Frame::kOk) {
    return true;
  }
  char bytes[48];
  Describe(rep->raw, rep->raw_len, bytes, sizeof(bytes));
  HCP_LOGW(hcp::log::kDomainModbus,
           "%s unit %u fc 0x%02x addr %u n %u -> %s (%u bytes: %s)", what,
           req.unit, req.fc, req.start, count, Name(f), (unsigned)rep->raw_len,
           bytes);
  return false;
}

bool Master::ReadBlock(uint8_t addr, uint16_t reg, uint16_t count, uint16_t* out,
                       uint8_t fc, const char* what) {
  if (count == 0 || count > kMaxRegsPerReply || out == nullptr) {
    last_error_ = -EFBIG;
    return false;
  }
  Request req;
  req.unit = addr;
  req.fc = fc;
  req.start = reg;
  req.count = count;

  Reply rep;
  if (!Transceive(req, &rep, what, count)) {
    return false;
  }
  if (rep.reg_count > count) {
    last_error_ = -EMSGSIZE;  // more than we asked for is not something to copy
    return false;
  }
  memcpy(out, rep.regs, rep.reg_count * sizeof(out[0]));
  return true;
}

bool Master::ReadHoldingRegisters(uint8_t addr, uint16_t reg, uint16_t count,
                                 uint16_t* out) {
  return ReadBlock(addr, reg, count, out, kFcReadHolding, "read holding");
}

bool Master::ReadInputRegisters(uint8_t addr, uint16_t reg, uint16_t count,
                               uint16_t* out) {
  return ReadBlock(addr, reg, count, out, kFcReadInput, "read input");
}

bool Master::WriteSingleRegister(uint8_t addr, uint16_t reg, uint16_t value) {
  Request req;
  req.unit = addr;
  req.fc = kFcWriteSingle;
  req.start = reg;
  req.count = 1;
  req.values = &value;
  Reply rep;
  return Transceive(req, &rep, "write", 1);
}

bool Master::WriteMultipleRegisters(uint8_t addr, uint16_t reg, uint16_t count,
                                   const uint16_t* values) {
  if (count == 0 || count > kMaxRegsPerReply || values == nullptr) {
    last_error_ = -EFBIG;
    return false;
  }
  Request req;
  req.unit = addr;
  req.fc = kFcWriteMultiple;
  req.start = reg;
  req.count = count;
  req.values = values;
  Reply rep;
  return Transceive(req, &rep, "write", count);
}

bool Master::WriteCoil(uint8_t addr, uint16_t reg, bool on) {
  Request req;
  req.unit = addr;
  req.fc = kFcWriteCoil;
  req.start = reg;
  req.count = 1;
  req.coil_on = on;
  Reply rep;
  return Transceive(req, &rep, "write coil", 1);
}

bool Master::Transceive645(const uint8_t* frame, size_t len) {
  if (!impl_->open) {
    last_error_ = -1;
    return false;
  }
  if (!IsDlt645(frame, len)) {
    last_error_ = -EMSGSIZE;
    return false;
  }

  const serial::Port::Line modbus = impl_->port.line();
  serial::Port::Line dlt;
  dlt.baud = kDlt645Baud;
  dlt.parity = serial::Port::Parity::kEven;
  dlt.stop_bits = 1;
  if (!impl_->port.SetLine(dlt)) {
    last_error_ = -EIO;
    HCP_LOGW(hcp::log::kDomainModbus, "645: uart refused 2400 8E1");
    return false;
  }

  Reply645 rep;
  const Frame645 f =
      Transact645(impl_->io, frame, len, impl_->cfg.response_timeout_ms, &rep);

  /* Restored before anything else can run on this thread's bus, and on every
   * outcome: leaving 2400 8E1 behind would silently break the next Modbus poll. */
  if (!impl_->port.SetLine(modbus)) {
    HCP_LOGE(hcp::log::kDomainModbus, "645: could not restore %u baud",
             (unsigned)modbus.baud);
  }

  last_error_ = Errno645(f);
  if (f == Frame645::kOk) {
    return true;
  }
  char bytes[48];
  Describe(rep.raw, rep.raw_len, bytes, sizeof(bytes));
  HCP_LOGW(hcp::log::kDomainModbus, "645 (%u bytes) -> %s (%u bytes: %s)",
           (unsigned)len, Name645(f), (unsigned)rep.raw_len, bytes);
  return false;
}

}  // namespace modbus
}  // namespace hcp
