/*
 * Host tests for the DL/T645-2007 framing that shares the RS485 pair with Modbus.
 *
 * The request vector is the real 30-byte "switch this meter to Modbus" frame
 * captured off the platform (wiki iotrix-app-protocol.md §8.8), byte for byte. Its
 * 表号 is a value the user typed during that test, not a real meter's number - it is
 * used here only as a checksummed address, and nothing is inferred from it.
 *
 * Copyright (c) 2026 maybetaken
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <gtest/gtest.h>

#include <string.h>

#include <cstdlib>
#include <string>
#include <vector>

#include "hcp/modbus/dlt645.h"

using namespace hcp::modbus;

namespace {

/* Scripted (delay, bytes) source, as in test_rtu.cpp: `now_` only moves when a read
 * consumes time, so a 450 ms slave costs the test no real sleep. */
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
      now_ += timeout_ms;
      return 0;
    }
    Chunk& c = script_[next_];
    if (c.delay > timeout_ms) {
      now_ += timeout_ms;
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

 private:
  std::vector<Chunk> script_;
  size_t next_ = 0;
  uint64_t now_ = 0;
  std::vector<uint8_t> written_;
};

std::vector<uint8_t> Bytes(const std::string& hex) {
  std::string compact;
  for (char c : hex) {
    if (c != ' ') {
      compact.push_back(c);
    }
  }
  std::vector<uint8_t> v;
  for (size_t i = 0; i + 1 < compact.size(); i += 2) {
    v.push_back(
        static_cast<uint8_t>(strtoul(compact.substr(i, 2).c_str(), nullptr, 16)));
  }
  return v;
}

/* The captured switch frame: FE×4, 表号 01 91 78 56 34 12, ctrl 0x14 (write), 14 data
 * bytes each +0x33, CS 0x70, 0x16. */
const char* kSwitchHex =
    "fe fe fe fe 68 01 91 78 56 34 12 68 14 0e 33 33 35 3d 35 33 33 33 33 33 33 33 33 33 70 16";

/* A normal reply: same address, ctrl 0x94 (0x14 | 0x80), no data. */
const char* kAckHex = "68 01 91 78 56 34 12 68 94 00 0a 16";
/* ctrl 0xD4 = 0x94 | 0x40: the slave refused. */
const char* kErrHex = "68 01 91 78 56 34 12 68 d4 01 33 7e 16";

/* Append the checksum and terminator to a body that starts at the first 0x68. */
std::vector<uint8_t> Seal(std::vector<uint8_t> body) {
  uint8_t sum = 0;
  for (uint8_t b : body) {
    sum = static_cast<uint8_t>(sum + b);
  }
  body.push_back(sum);
  body.push_back(0x16);
  return body;
}

}  // namespace

TEST(Dlt645Classify, AcceptsTheCapturedSwitchFrame) {
  const auto f = Bytes(kSwitchHex);
  ASSERT_EQ(30u, f.size());
  EXPECT_TRUE(IsDlt645(f.data(), f.size()));
  EXPECT_EQ(f.size(), Dlt645FrameLen(f.data(), f.size()));
}

TEST(Dlt645Classify, RefusesTheCapturedModbusWrites) {
  /* The three FC10 frames from the same capture round. 0xFE is 254, which is not a
   * legal Modbus unit, so the preamble alone separates the two protocols. */
  for (const char* hex : {"03 10 00 02 00 01 02 00 01 7f 12",
                          "05 10 00 06 00 01 02 00 03 d4 f7",
                          "03 10 00 05 00 01 02 00 05 7f 66"}) {
    const auto f = Bytes(hex);
    EXPECT_FALSE(IsDlt645(f.data(), f.size())) << hex;
  }
}

TEST(Dlt645Classify, RefusesBadChecksum) {
  auto f = Bytes(kSwitchHex);
  f[f.size() - 2] ^= 0xFF;
  EXPECT_FALSE(IsDlt645(f.data(), f.size()));
}

TEST(Dlt645Classify, RefusesTrailingOrMissingBytes) {
  const auto f = Bytes(kSwitchHex);
  EXPECT_FALSE(IsDlt645(f.data(), f.size() - 1));  // terminator gone
  auto longer = f;
  longer.push_back(0x00);
  EXPECT_FALSE(IsDlt645(longer.data(), longer.size()));  // not exactly one frame
}

TEST(Dlt645Classify, AcceptsOneToFourPreambleBytes) {
  const auto f = Bytes(kSwitchHex);
  const std::vector<uint8_t> body(f.begin() + 4, f.end());
  for (int fe = 1; fe <= 4; fe++) {
    std::vector<uint8_t> v(static_cast<size_t>(fe), 0xFE);
    v.insert(v.end(), body.begin(), body.end());
    EXPECT_TRUE(IsDlt645(v.data(), v.size())) << "preamble " << fe;
  }
  /* Five is past what the standard allows, and the fifth 0xFE is then read as the
   * start marker, which it is not. */
  std::vector<uint8_t> five(5, 0xFE);
  five.insert(five.end(), body.begin(), body.end());
  EXPECT_FALSE(IsDlt645(five.data(), five.size()));
}

TEST(Dlt645Classify, RefusesMissingSecondStartMarker) {
  auto f = Bytes(kSwitchHex);
  f[11] = 0x00;  // the 0x68 after the address
  EXPECT_FALSE(IsDlt645(f.data(), f.size()));
}

TEST(Dlt645Classify, RefusesLengthFieldThatDisagrees) {
  auto f = Bytes(kSwitchHex);
  f[13] = 0x0D;  // claims one data byte fewer than the frame carries
  EXPECT_FALSE(IsDlt645(f.data(), f.size()));
}

TEST(Dlt645FrameLen, NeedsTheWholeHeaderBeforeItCanSize) {
  const auto f = Bytes(kSwitchHex);
  for (size_t n = 0; n < 14; n++) {
    EXPECT_EQ(0u, Dlt645FrameLen(f.data(), n)) << "n=" << n;
  }
  EXPECT_EQ(30u, Dlt645FrameLen(f.data(), 14));
}

TEST(Dlt645FrameLen, ToleratesAReplyWithoutPreamble) {
  const auto r = Bytes(kAckHex);
  EXPECT_EQ(r.size(), Dlt645FrameLen(r.data(), r.size()));
}

TEST(Dlt645Transact, SendsTheFrameVerbatimAndAcceptsTheAck) {
  FakeIo io;
  const auto f = Bytes(kSwitchHex);
  io.Feed(200, Bytes(kAckHex));
  Reply645 rep;
  EXPECT_EQ(Frame645::kOk, Transact645(io, f.data(), f.size(), 1000, &rep));
  EXPECT_EQ(f, io.written());  // verbatim: checksum and terminator included
  EXPECT_EQ(0x94, rep.ctrl);
  EXPECT_EQ(12u, rep.raw_len);
}

TEST(Dlt645Transact, WaitsForASlowMeter) {
  FakeIo io;
  const auto f = Bytes(kSwitchHex);
  io.Feed(450, Bytes(kAckHex));
  Reply645 rep;
  EXPECT_EQ(Frame645::kOk, Transact645(io, f.data(), f.size(), 1000, &rep));
}

TEST(Dlt645Transact, DiscardsOurOwnEcho) {
  FakeIo io;
  const auto f = Bytes(kSwitchHex);
  io.Feed(0, f);  // half-duplex line hands the request straight back
  io.Feed(200, Bytes(kAckHex));
  Reply645 rep;
  EXPECT_EQ(Frame645::kOk, Transact645(io, f.data(), f.size(), 1000, &rep));
  EXPECT_EQ(0x94, rep.ctrl);
}

TEST(Dlt645Transact, ReportsTheSlavesRefusal) {
  FakeIo io;
  const auto f = Bytes(kSwitchHex);
  io.Feed(100, Bytes(kErrHex));
  Reply645 rep;
  EXPECT_EQ(Frame645::kError, Transact645(io, f.data(), f.size(), 1000, &rep));
  EXPECT_EQ(0xD4, rep.ctrl);
}

TEST(Dlt645Transact, NothingArrived) {
  FakeIo io;
  const auto f = Bytes(kSwitchHex);
  Reply645 rep;
  EXPECT_EQ(Frame645::kNoReply, Transact645(io, f.data(), f.size(), 1000, &rep));
  EXPECT_EQ(0u, rep.raw_len);
}

TEST(Dlt645Transact, PartialReplyIsTruncatedNotOk) {
  FakeIo io;
  const auto f = Bytes(kSwitchHex);
  const auto r = Bytes(kAckHex);
  io.Feed(100, std::vector<uint8_t>(r.begin(), r.begin() + 5));
  Reply645 rep;
  EXPECT_EQ(Frame645::kTruncated, Transact645(io, f.data(), f.size(), 1000, &rep));
  EXPECT_EQ(5u, rep.raw_len);  // the bytes are kept for the log
}

TEST(Dlt645Transact, BadChecksumIsNotSuccess) {
  FakeIo io;
  const auto f = Bytes(kSwitchHex);
  auto r = Bytes(kAckHex);
  r[r.size() - 2] ^= 0xFF;
  io.Feed(100, r);
  Reply645 rep;
  EXPECT_EQ(Frame645::kBadCs, Transact645(io, f.data(), f.size(), 1000, &rep));
}

TEST(Dlt645Transact, AnotherMetersAnswerIsNotOurs) {
  FakeIo io;
  const auto f = Bytes(kSwitchHex);
  auto r = Seal(Bytes("68 02 91 78 56 34 12 68 94 00"));
  io.Feed(100, r);
  Reply645 rep;
  EXPECT_EQ(Frame645::kWrongAddr, Transact645(io, f.data(), f.size(), 1000, &rep));
}

TEST(Dlt645Transact, RefusesToSendAnythingThatIsNotA645Frame) {
  FakeIo io;
  const auto f = Bytes("03 10 00 05 00 01 02 00 05 7f 66");
  Reply645 rep;
  EXPECT_EQ(Frame645::kBadFrame, Transact645(io, f.data(), f.size(), 1000, &rep));
  EXPECT_TRUE(io.written().empty());  // never reached the wire
}