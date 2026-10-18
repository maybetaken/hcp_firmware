/*
 * hcp::switchctl — POSIX backend (host). In-memory fake relay state.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "hcp/switch/relay.h"

namespace hcp {
namespace switchctl {

struct Relay::Impl {
  bool bound = false;
};

Relay::Relay() : impl_(new Impl()) {}
Relay::~Relay() { delete impl_; }

bool Relay::Open(int index) {
  if (index < 0 || index >= kRelayCount) {
    return false;
  }
  impl_->bound = true;
  index_ = index;
  state_ = false;
  return true;
}

void Relay::Set(bool on) {
  if (!impl_->bound) {
    return;
  }
  state_ = on;
}

bool Relay::Get() const { return state_; }

}  // namespace switchctl
}  // namespace hcp
