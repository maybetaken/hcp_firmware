/*
 * HCP switch control — relay outputs K1/K2 (devicetree aliases hcp-k1/hcp-k2).
 * Backend: zephyr (gpio) on target, in-memory fake on host. Google C++ naming.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

namespace hcp {
namespace switchctl {

constexpr int kRelayCount = 2;

class Relay {
 public:
  Relay();
  ~Relay();
  Relay(const Relay&) = delete;
  Relay& operator=(const Relay&) = delete;

  bool Open(int index);  // 0 = K1, 1 = K2
  void Set(bool on);
  bool Get() const;
  int Index() const { return index_; }

 private:
  struct Impl;
  Impl* impl_;
  int index_ = -1;
  bool state_ = false;
};

}  // namespace switchctl
}  // namespace hcp
