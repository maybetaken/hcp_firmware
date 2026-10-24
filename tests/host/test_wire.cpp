/*
 * Host tests for hcp::registry wire packing - the byte layout Home Assistant's
 * modbus_protocol_helper decodes with struct.unpack(">BBHH", ...) plus big-endian
 * data words. Vectors come from the real device_protocol payloads (ddsu666: unit 1
 * start 8192; jkbms: unit 15 start 5120) so a "cleanup" that changes the byte
 * order fails here instead of shipping garbage values.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <gtest/gtest.h>

#include <string.h>

#include "hcp/registry/wire.h"

using namespace hcp::registry;

namespace {

std::string Hex(const uint8_t* b, size_t n) {
  static const char* d = "0123456789ABCDEF";
  std::string s;
  for (size_t i = 0; i < n; i++) {
    s += d[b[i] >> 4];
    s += d[b[i] & 0xF];
    if (i + 1 < n) s += ' ';
  }
  return s;
}

}  // namespace

TEST(WirePack, RegistersAreBigEndianAfterABigEndianHeader) {
  // DDSU666 first segment: unit 1, FC03, start 8192 (0x2000), 2 registers read.
  Segment seg{1, 0x2000, 2, 0x03};
  const uint16_t regs[2] = {0x0001, 0x0203};
  uint8_t out[32] = {};

  const size_t n = PackRegisters(seg, regs, 2, out, sizeof(out));
  ASSERT_EQ(n, 10u);
  EXPECT_EQ(Hex(out, n), "01 03 20 00 00 02 00 01 02 03");
}

TEST(WirePack, UnitAndAddressComeFromTheSegment) {
  // jkbms.json: unit 15, start 5120 (0x1400), 1 register here.
  Segment seg{15, 0x1400, 1, 0x03};
  const uint16_t regs[1] = {0xABCD};
  uint8_t out[16] = {};
  const size_t n = PackRegisters(seg, regs, 1, out, sizeof(out));
  ASSERT_EQ(n, 8u);
  EXPECT_EQ(Hex(out, n), "0F 03 14 00 00 01 AB CD");
}

TEST(WirePack, FC04IsCarriedThroughAsReadCommand) {
  // pzem_v04.json's first segment reads input registers (FC04).
  Segment seg{1, 0x0000, 1, 0x04};
  const uint16_t regs[1] = {0x1234};
  uint8_t out[16] = {};
  const size_t n = PackRegisters(seg, regs, 1, out, sizeof(out));
  ASSERT_EQ(n, 8u);
  EXPECT_EQ(Hex(out, n), "01 04 00 00 00 01 12 34");
}

TEST(WirePack, BitsArePackedMSBFirstAsTheResponseCarriesThem) {
  Segment seg{1, 0x0000, 10, 0x01};  // 10 coils -> 2 bytes
  const uint8_t bits[2] = {0xFF, 0x03};
  uint8_t out[16] = {};
  const size_t n = PackBits(seg, bits, 10, out, sizeof(out));
  ASSERT_EQ(n, 8u);
  EXPECT_EQ(Hex(out, n), "01 01 00 00 00 0A FF 03");
}

TEST(WirePack, RefusesToOverflowOrToDisagreeWithTheTable) {
  Segment seg{1, 0x2000, 4, 0x03};
  const uint16_t regs[4] = {1, 2, 3, 4};
  uint8_t out[16] = {};

  // header + 8 data bytes = 14; 13 must not be reported as success
  EXPECT_EQ(PackRegisters(seg, regs, 4, out, 13), 0u);
  EXPECT_EQ(PackRegisters(seg, regs, 4, out, 14), 14u);

  // the caller says it read 3 for a segment of 4: the table and the read disagree
  EXPECT_EQ(PackRegisters(seg, regs, 3, out, sizeof(out)), 0u);
  EXPECT_EQ(PackBits(seg, nullptr, 4, out, sizeof(out)), 0u);
}

TEST(LedCommand, TakesWhatHomeAssistantSends) {
  /* base_device.set_led publishes the bare words "on"/"off" on <sn>/control/led;
   * 2 and 3 bytes are the two lengths seen on the real device. */
  const uint8_t on[2] = {'o', 'n'};
  const uint8_t off[3] = {'o', 'f', 'f'};
  EXPECT_EQ(ParseLedCommand(on, 2), LedSetting::kOn);
  EXPECT_EQ(ParseLedCommand(off, 3), LedSetting::kOff);

  // hand-written automations add a newline and mixed case
  const uint8_t on_nl[3] = {'o', 'n', '\n'};
  EXPECT_EQ(ParseLedCommand(on_nl, 3), LedSetting::kOn);
  const uint8_t mixed[3] = {'O', 'F', 'F'};
  EXPECT_EQ(ParseLedCommand(mixed, 3), LedSetting::kOff);
}

TEST(LedCommand, RejectsAnythingElse) {
  const uint8_t toggle[6] = {'t', 'o', 'g', 'g', 'l', 'e'};
  EXPECT_EQ(ParseLedCommand(toggle, 6), LedSetting::kInvalid);
  const uint8_t true_[4] = {'t', 'r', 'u', 'e'};
  EXPECT_EQ(ParseLedCommand(true_, 4), LedSetting::kInvalid);
  const uint8_t one[1] = {'o'};
  EXPECT_EQ(ParseLedCommand(one, 1), LedSetting::kInvalid);
  EXPECT_EQ(ParseLedCommand(nullptr, 0), LedSetting::kInvalid);
  // a longer payload that merely starts with "on" is not an LED command
  const uint8_t onx[4] = {'o', 'n', 'x', '!'};
  EXPECT_EQ(ParseLedCommand(onx, 4), LedSetting::kInvalid);
}

/* --------------------------------------------------------- control/cmd decode -- */

TEST(WireParse, SingleRegisterWrite) {
  const uint8_t buf[6] = {0x01, 0x06, 0x00, 0x2A, 0x12, 0x34};
  WriteRequest req{};
  ASSERT_EQ(ParseWriteRequest(buf, sizeof(buf), &req), WriteParse::kOk);
  EXPECT_EQ(req.unit_id, 1);
  EXPECT_EQ(req.function, 0x06);
  EXPECT_EQ(req.start_address, 0x2A);
  EXPECT_EQ(req.count, 1);
  EXPECT_EQ(req.values[0], 0x1234);
}

TEST(WireParse, MultipleRegisterWrite) {
  const uint8_t buf[10] = {0x0F, 0x10, 0x14, 0x00, 0x00, 0x02,
                           0x00, 0x01, 0x00, 0x02};
  WriteRequest req{};
  ASSERT_EQ(ParseWriteRequest(buf, sizeof(buf), &req), WriteParse::kOk);
  EXPECT_EQ(req.unit_id, 15);
  EXPECT_EQ(req.start_address, 0x1400);
  EXPECT_EQ(req.count, 2);
  EXPECT_EQ(req.values[0], 1);
  EXPECT_EQ(req.values[1], 2);
}

TEST(WireParse, CoilWriteIsDecodedIntoABool) {
  const uint8_t on[6] = {0x01, 0x05, 0x00, 0x00, 0xFF, 0x00};
  WriteRequest req{};
  ASSERT_EQ(ParseWriteRequest(on, sizeof(on), &req), WriteParse::kOk);
  EXPECT_EQ(req.function, 0x05);
  EXPECT_TRUE(req.coil_on);

  const uint8_t off[6] = {0x01, 0x05, 0x00, 0x00, 0x00, 0x00};
  ASSERT_EQ(ParseWriteRequest(off, sizeof(off), &req), WriteParse::kOk);
  EXPECT_FALSE(req.coil_on);

  // anything but 0x0000/0xFF00 is an illegal Modbus coil value
  const uint8_t bogus[6] = {0x01, 0x05, 0x00, 0x00, 0x12, 0x34};
  EXPECT_EQ(ParseWriteRequest(bogus, sizeof(bogus), &req), WriteParse::kBad);
}

TEST(WireParse, RejectsWhatItCannotTrust) {
  WriteRequest req{};
  const uint8_t short_[5] = {1, 6, 0, 0, 0};
  EXPECT_EQ(ParseWriteRequest(short_, 5, &req), WriteParse::kBad);

  const uint8_t unknown_fc[6] = {1, 0x99, 0, 0, 0, 1};
  EXPECT_EQ(ParseWriteRequest(unknown_fc, 6, &req), WriteParse::kBad);

  // claims 100 registers but carries no data: a header alone must not pass
  const uint8_t lying[6] = {1, 0x10, 0x00, 0x00, 0x00, 0x64};
  EXPECT_EQ(ParseWriteRequest(lying, 6, &req), WriteParse::kBad);

  // broadcast unit 0: a write there cannot be answered, a read never should
  const uint8_t bcast[6] = {0x00, 0x06, 0x00, 0x00, 0x00, 0x01};
  EXPECT_EQ(ParseWriteRequest(bcast, 6, &req), WriteParse::kBad);

  EXPECT_EQ(ParseWriteRequest(nullptr, 0, &req), WriteParse::kBad);

  // more values than the struct can hold is refused, not truncated
  uint8_t many[6 + 2 * (kMaxWriteValues + 1)] = {1, 0x10, 0, 0,
                                                 0, kMaxWriteValues + 1};
  EXPECT_EQ(ParseWriteRequest(many, sizeof(many), &req), WriteParse::kBad);
}
