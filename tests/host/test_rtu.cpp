/*
 * Host tests for the RTU frame engine. A scripted transport lets us reproduce the
 * cases that took hours on the bench - a slave whose first byte arrives 450 ms
 * after the request, an echo of our own request on a half-duplex line, stale bytes
 * left by a transaction we gave up on - and assert them in microseconds.
 *
 * Copyright (c) 2026 maybetaken
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <gtest/gtest.h>

#include <string.h>

#include <vector>

#include "hcp/modbus/rtu.h"

using namespace hcp::modbus;

namespace {

/* A reply as the wire carries it: payload plus CRC, low byte first. */
std::vector<uint8_t> WithCrc(std::vector<uint8_t> body) {
  const uint16_t crc = Crc16(body.data(), body.size());
  body.push_back(static_cast<uint8_t>(crc & 0xFF));
  body.push_back(static_cast<uint8_t>(crc >> 8));
  return body;
}

/* Scripted (delay, bytes) source. `now_` only moves when a read consumes time, so a
 * 450 ms slave costs the test no real sleep. */
class FakeIo : public ByteIo {
 public:
  struct Chunk {
    uint64_t delay;
    std::vector<uint8_t> data;
    size_t pos = 0;
  };

  void Feed(uint64_t delay_ms, const std::vector<uint8_t>& bytes) {
    script_.push_back({delay_ms, bytes, 0});
  }

  void Write(const uint8_t* data, size_t len) override {
    written_.assign(data, data + len);
  }

  int Read(uint8_t* buf, size_t cap, uint32_t timeout_ms) override {
    if (next_ >= script_.size()) {
      now_ += timeout_ms;  // nothing left to deliver: the wait expires
      return 0;
    }
    Chunk& c = script_[next_];
    if (c.delay > timeout_ms) {
      now_ += timeout_ms;  // it appears only after the caller's budget is gone
      return 0;
    }
    now_ += c.delay;
    c.delay = 0;
    const size_t n = cap < c.data.size() - c.pos ? cap : c.data.size() - c.pos;
    memcpy(buf, c.data.data() + c.pos, n);
    c.pos += n;
    if (c.pos == c.data.size()) {
      next_++;
    }
    return static_cast<int>(n);
  }

  uint64_t NowMs() override { return now_; }

  const std::vector<uint8_t>& written() const { return written_; }
  uint64_t now() const { return now_; }

 private:
  std::vector<Chunk> script_;
  size_t next_ = 0;
  uint64_t now_ = 0;
  std::vector<uint8_t> written_;
};

Request ReadReq(uint8_t fc = kFcReadInput, uint16_t start = 1, uint16_t count = 12) {
  Request r;
  r.unit = 1;
  r.fc = fc;
  r.start = start;
  r.count = count;
  return r;
}

/* 24 data bytes for a 12-register read, with recognisable values. */
std::vector<uint8_t> GoodReadReply(uint8_t unit = 1, uint8_t fc = kFcReadInput) {
  std::vector<uint8_t> body = {unit, fc, 24};
  for (uint16_t i = 0; i < 12; i++) {
    body.push_back(static_cast<uint8_t>(0x11 * (i + 1) >> 8));
    body.push_back(static_cast<uint8_t>(0x11 * (i + 1) & 0xFF));
  }
  return WithCrc(body);
}

}  // namespace

/* ------------------------------------------------------------------ CRC, real -- */

TEST(RtuCrc, MatchesWhatTheMeterAcceptedOnTheWire) {
  /* The request the bench meter answered (it replied for hours to exactly these
   * bytes): 01 04 00 01 00 0C + CRC A1 CF, low byte first. Pinned because every
   * other test builds on this function. */
  const uint8_t req[6] = {0x01, 0x04, 0x00, 0x01, 0x00, 0x0C};
  EXPECT_EQ(Crc16(req, sizeof(req)), 0xCFA1);

  Request r = ReadReq();
  uint8_t out[16] = {};
  const size_t n = BuildRequest(r, out, sizeof(out));
  ASSERT_EQ(n, 8u);
  EXPECT_EQ(out[6], 0xA1);
  EXPECT_EQ(out[7], 0xCF);
}

TEST(RtuBuild, RefusesWhatItCannotSend) {
  uint8_t out[300] = {};
  Request r = ReadReq(kFcReadHolding, 0, 1);
  r.unit = 0;  // broadcast: nothing can answer
  EXPECT_EQ(BuildRequest(r, out, sizeof(out)), 0u);

  r = ReadReq(kFcReadHolding, 0, 9999);  // more registers than one ADU holds
  EXPECT_EQ(BuildRequest(r, out, sizeof(out)), 0u);

  r = ReadReq(kFcWriteMultiple, 0, 2);
  r.values = nullptr;
  EXPECT_EQ(BuildRequest(r, out, sizeof(out)), 0u);

  r = ReadReq(0x2B, 0, 1);  // a function code this master does not send
  EXPECT_EQ(BuildRequest(r, out, sizeof(out)), 0u);
}

/* --------------------------------------------------------------- framing ---- */

TEST(RtuFraming, SizedByFunctionCodeNotByIdleLine) {
  EXPECT_EQ(ExpectedReplyLen(kFcReadInput, 24), 29u);   // 3 + 24 + 2
  EXPECT_EQ(ExpectedReplyLen(kFcReadCoils, 2), 7u);
  EXPECT_EQ(ExpectedReplyLen(kFcWriteSingle, 0), 8u);   // echo
  EXPECT_EQ(ExpectedReplyLen(0x84, 0), 5u);             // exception
  EXPECT_EQ(ExpectedReplyLen(0x2B, 0), 0u);             // unknown
}

TEST(RtuTransact, ReadsAGoodReplyAndKeepsRegisterOrder) {
  FakeIo io;
  io.Feed(450, GoodReadReply());  // the bench meter's real turnaround

  Reply rep;
  ASSERT_EQ(Transact(io, ReadReq(), 1000, &rep), Frame::kOk);
  EXPECT_EQ(rep.reg_count, 12);
  EXPECT_EQ(rep.regs[0], 0x0011);
  EXPECT_EQ(rep.regs[8], 0x0099);   // register 9 = 0x11 * 9, big-endian on the wire
  EXPECT_EQ(rep.raw_len, 29u);
  EXPECT_GE(io.now(), 450u);        // the deadline budget is spent waiting, not sleeping
}

TEST(RtuTransact, SlowSlaveWithinBudgetIsFineSlowSlaveBeyondIsNot) {
  FakeIo quick;
  quick.Feed(480, GoodReadReply());
  Reply rep;
  EXPECT_EQ(Transact(quick, ReadReq(), 1000, &rep), Frame::kOk);

  FakeIo late;
  late.Feed(900, GoodReadReply());   // appears after the 500 ms budget is gone
  Reply rep2;
  EXPECT_EQ(Transact(late, ReadReq(), 500, &rep2), Frame::kNoReply);
  EXPECT_EQ(rep2.raw_len, 0u);
}

TEST(RtuTransact, StaleBytesFromAnOlderTransactionAreFlushed) {
  /* The failure this engine exists for: a reply that arrived after we gave up on
   * the previous request sits in the receiver and shifts the next frame. */
  FakeIo io;
  std::vector<uint8_t> stale = {0x01, 0x04, 0x18, 0x00, 0x00};
  io.Feed(0, stale);
  io.Feed(300, GoodReadReply());

  Reply rep;
  ASSERT_EQ(Transact(io, ReadReq(), 1000, &rep), Frame::kOk);
  EXPECT_EQ(rep.reg_count, 12);
}

TEST(RtuTransact, HalfDuplexEchoOfOurOwnRequestIsDiscarded) {
  /* Port's Write returns only after the shift register is empty, so an echo of the
   * request is necessarily already in the receiver when we get back - delay 0 is
   * the physically correct model here, not a shortcut. */
  FakeIo io;
  Request r = ReadReq();
  uint8_t req[16] = {};
  const size_t n = BuildRequest(r, req, sizeof(req));
  io.Feed(0, std::vector<uint8_t>(req, req + n));  // we hear ourselves
  io.Feed(220, GoodReadReply());

  Reply rep;
  ASSERT_EQ(Transact(io, r, 1000, &rep), Frame::kOk);
  EXPECT_EQ(rep.reg_count, 12);
}

/* Note on what is deliberately not tested here: an echo that arrives *after* Write
 * returns. Port's Write blocks until the TX shift register is empty (it has to, for
 * DE), so such a timing does not exist on this hardware - and the request has a
 * valid CRC of its own, so if it ever did happen it would mis-frame and cost one
 * poll, after which the pre-transaction flush above recovers by itself. Asserting
 * behaviour for an impossible clock would only hide that reasoning. */

TEST(RtuTransact, ByteCountFieldThatDisagreesIsRefused) {
  FakeIo io;
  std::vector<uint8_t> wrong = GoodReadReply();
  wrong[2] = 22;  // claims 22 bytes of data while the frame carries 24
  // rebuild the CRC so only the count field is inconsistent
  const uint16_t c = Crc16(wrong.data(), wrong.size() - 2);
  wrong[wrong.size() - 2] = static_cast<uint8_t>(c & 0xFF);
  wrong[wrong.size() - 1] = static_cast<uint8_t>(c >> 8);
  io.Feed(2, wrong);

  Reply rep;
  EXPECT_EQ(Transact(io, ReadReq(), 1000, &rep), Frame::kTimeoutBogus);
}

TEST(RtuTransact, RealReplyIsNeverMistakenForEcho) {
  /* A slave that answers 2 ms after our last stop bit: the reply begins with the
   * same two bytes as the request (unit, function code), so echo suppression has
   * to compare the whole request length before it discards anything. */
  FakeIo io;
  io.Feed(2, GoodReadReply());
  Reply rep;
  ASSERT_EQ(Transact(io, ReadReq(), 1000, &rep), Frame::kOk);
}

TEST(RtuTransact, WriteEchoArrivingImmediatelyIsTheReply) {
  /* FC05/06/16: the slave echoes the request, so the suppression that reads must
   * not apply to writes - otherwise a successful write reads as a timeout. */
  FakeIo io;
  const uint16_t value = 0x1234;
  Request r = ReadReq(kFcWriteSingle, 0x2A2B, 1);
  r.values = &value;
  uint8_t req[16] = {};
  const size_t n = BuildRequest(r, req, sizeof(req));
  io.Feed(1, std::vector<uint8_t>(req, req + n));

  Reply rep;
  ASSERT_EQ(Transact(io, r, 1000, &rep), Frame::kOk);
}

TEST(RtuTransact, ExceptionReplyIsReportedAsSuch) {
  FakeIo io;
  io.Feed(5, WithCrc({0x01, 0x84, 0x02}));  // FC04 + exception bit, code 2
  Reply rep;
  ASSERT_EQ(Transact(io, ReadReq(), 1000, &rep), Frame::kException);
  EXPECT_EQ(rep.exception_code, 2);
  EXPECT_EQ(rep.fc, 0x84);
}

TEST(RtuTransact, BadCrcKeepsTheBytesItSaw) {
  FakeIo io;
  std::vector<uint8_t> bad = GoodReadReply();
  bad[5] ^= 0xFF;  // corrupt a data byte, leave the CRC as the slave computed it
  io.Feed(5, bad);

  Reply rep;
  ASSERT_EQ(Transact(io, ReadReq(), 1000, &rep), Frame::kBadCrc);
  EXPECT_EQ(rep.raw_len, 29u);
  EXPECT_EQ(rep.raw[0], 0x01);
  EXPECT_EQ(rep.raw[1], 0x04);
}

TEST(RtuTransact, AnotherUnitAnsweringIsNotOurs) {
  FakeIo io;
  io.Feed(5, GoodReadReply(/*unit=*/7));
  Reply rep;
  EXPECT_EQ(Transact(io, ReadReq(), 1000, &rep), Frame::kWrongUnit);
  EXPECT_EQ(rep.unit, 7);
}

TEST(RtuTransact, HalfAFrameIsTruncatedNotMisread) {
  FakeIo io;
  std::vector<uint8_t> all = GoodReadReply();
  all.resize(11);  // first 11 bytes only, then silence until the deadline
  io.Feed(5, all);

  Reply rep;
  EXPECT_EQ(Transact(io, ReadReq(), 1000, &rep), Frame::kTruncated);
  EXPECT_EQ(rep.raw_len, 11u);
}

TEST(RtuTransact, BitReadKeepsThePackedBytes) {
  FakeIo io;
  Request r = ReadReq(kFcReadCoils, 0, 10);
  io.Feed(5, WithCrc({0x01, 0x01, 0x02, 0xFF, 0x03}));  // 10 coils in 2 bytes

  Reply rep;
  ASSERT_EQ(Transact(io, r, 1000, &rep), Frame::kOk);
  EXPECT_EQ(rep.reg_count, 10);  // bits, not bytes
  EXPECT_EQ(rep.bits[0], 0xFF);
  EXPECT_EQ(rep.bits[1], 0x03);
}

TEST(RtuTransact, WriteEchoCompletesTheTransaction) {
  FakeIo io;
  Request r = ReadReq(kFcWriteSingle, 0x2A2B, 1);
  const uint16_t value = 0x1234;
  r.values = &value;
  uint8_t req[16] = {};
  const size_t n = BuildRequest(r, req, sizeof(req));
  ASSERT_EQ(n, 8u);
  io.Feed(4, std::vector<uint8_t>(req, req + n));  // the echo that means "done"

  Reply rep;
  ASSERT_EQ(Transact(io, r, 1000, &rep), Frame::kOk);
  EXPECT_EQ(rep.raw_len, 8u);
}
