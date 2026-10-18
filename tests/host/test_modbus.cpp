/*
 * Host unit tests for hcp::modbus.
 *  - End-to-end RTU framing/CRC via the vendored nanoMODBUS client, driven with
 *    a deterministic canned transport (captured request + prebuilt response).
 *  - Master.open() smoke test on the posix loopback backend.
 * Mirrors master.cpp's platform read/write pattern, so it exercises our glue.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "hcp/modbus/master.h"

extern "C" {
#include "nanomodbus.h"
}

namespace {

// Deterministic transport: write() captures bytes; read() serves a preset.
struct TestPort {
  std::vector<uint8_t> sent;
  std::vector<uint8_t> recv;
  size_t rpos = 0;
};

int32_t TestRead(uint8_t* buf, uint16_t count, int32_t byte_timeout_ms, void* arg) {
  auto* p = static_cast<TestPort*>(arg);
  // byte_timeout_ms == 0 => non-blocking single read (used by nanoMODBUS to
  // discard stale bytes before a request). Nothing is pending yet -> return 0.
  if (byte_timeout_ms <= 0) {
    return 0;
  }
  uint16_t got = 0;
  while (got < count && p->rpos < p->recv.size()) {
    buf[got++] = p->recv[p->rpos++];
  }
  return got;
}

int32_t TestWrite(const uint8_t* buf, uint16_t count, int32_t /*to*/, void* arg) {
  auto* p = static_cast<TestPort*>(arg);
  p->sent.insert(p->sent.end(), buf, buf + count);
  return count;
}

// Reference Modbus CRC16 (poly 0xA001) used to build/canonicalize frames.
uint16_t Crc16(const uint8_t* data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int b = 0; b < 8; ++b) {
      crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : (crc >> 1);
    }
  }
  return crc;  // low byte first when appended
}

void AppendCrc(std::vector<uint8_t>& f) {
  uint16_t c = Crc16(f.data(), f.size());
  f.push_back(static_cast<uint8_t>(c & 0xFF));
  f.push_back(static_cast<uint8_t>(c >> 8));
}

nmbs_error ClientInit(nmbs_t* nmbs, TestPort* tp) {
  nmbs_platform_conf conf;
  nmbs_platform_conf_create(&conf);
  conf.transport = NMBS_TRANSPORT_RTU;
  conf.read = TestRead;
  conf.write = TestWrite;
  conf.arg = tp;
  return nmbs_client_create(nmbs, &conf);
}

}  // namespace

TEST(ModbusRtu, ReadHoldingRegisters_FramingAndCrc) {
  TestPort tp;
  nmbs_t nmbs;
  ASSERT_EQ(ClientInit(&nmbs, &tp), NMBS_ERROR_NONE);
  nmbs_set_read_timeout(&nmbs, 500);
  nmbs_set_byte_timeout(&nmbs, 50);
  nmbs_set_destination_rtu_address(&nmbs, 1);

  // Canned response: unit=1 fc=3 bytecount=6 regs {10,20,30} + crc.
  std::vector<uint8_t> resp = {1, 3, 6, 0, 10, 0, 20, 0, 30};
  AppendCrc(resp);
  tp.recv = resp;

  uint16_t regs[3] = {};
  nmbs_error err = nmbs_read_holding_registers(&nmbs, 2, 3, regs);
  EXPECT_EQ(err, NMBS_ERROR_NONE) << nmbs_strerror(err);
  EXPECT_EQ(regs[0], 10);
  EXPECT_EQ(regs[1], 20);
  EXPECT_EQ(regs[2], 30);

  // Captured request must be exactly the valid RTU frame.
  std::vector<uint8_t> want = {1, 3, 0, 2, 0, 3};
  AppendCrc(want);
  EXPECT_EQ(tp.sent, want);
}

TEST(ModbusRtu, WriteSingleRegister_Echo) {
  TestPort tp;
  nmbs_t nmbs;
  ASSERT_EQ(ClientInit(&nmbs, &tp), NMBS_ERROR_NONE);
  nmbs_set_read_timeout(&nmbs, 500);
  nmbs_set_byte_timeout(&nmbs, 50);
  nmbs_set_destination_rtu_address(&nmbs, 1);

  std::vector<uint8_t> req = {1, 6, 0, 7, 0, 0x56};
  AppendCrc(req);
  tp.recv = req;  // write-single echoes the request as the response

  nmbs_error err = nmbs_write_single_register(&nmbs, 7, 0x56);
  EXPECT_EQ(err, NMBS_ERROR_NONE) << nmbs_strerror(err);
  EXPECT_EQ(tp.sent, req);
}

TEST(ModbusRtu, RejectsBadCrc) {
  TestPort tp;
  nmbs_t nmbs;
  ASSERT_EQ(ClientInit(&nmbs, &tp), NMBS_ERROR_NONE);
  nmbs_set_read_timeout(&nmbs, 200);
  nmbs_set_byte_timeout(&nmbs, 50);
  nmbs_set_destination_rtu_address(&nmbs, 1);

  std::vector<uint8_t> resp = {1, 3, 2, 0, 10};
  AppendCrc(resp);
  resp.back() ^= 0xFF;  // corrupt crc
  tp.recv = resp;

  uint16_t regs[1] = {};
  EXPECT_NE(nmbs_read_holding_registers(&nmbs, 0, 1, regs), NMBS_ERROR_NONE);
}

TEST(ModbusMaster, OpenOnPosixLoopback) {
  hcp::modbus::Master m;
  hcp::modbus::Config cfg;
  cfg.baud = 9600;
  EXPECT_TRUE(m.Open(cfg));  // posix Port.Open always succeeds
  m.Close();
}
