/*
 * hcp::led — POSIX backend (host). CPU-side framebuffer only; Show() no-op.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "hcp/led/strip.h"

#include <vector>

namespace hcp {
namespace led {

struct Strip::Impl {
  std::vector<Color> fb;
};

Strip::Strip(uint16_t count) : impl_(new Impl()), count_(count) {
  impl_->fb.assign(count, Color{});
}
Strip::~Strip() { delete impl_; }

bool Strip::Open() {
  open_ = true;
  return true;
}

void Strip::SetAll(Color c) {
  for (auto& p : impl_->fb) {
    p = c;
  }
}

void Strip::SetPixel(uint16_t index, Color c) {
  if (index < count_) {
    impl_->fb[index] = c;
  }
}

bool Strip::GetPixel(uint16_t index, Color& out) const {
  if (index >= count_) {
    return false;
  }
  out = impl_->fb[index];
  return true;
}

void Strip::SetBrightness(uint8_t percent) {
  brightness_ = percent > 100 ? 100 : percent;
}

void Strip::Clear() {
  for (auto& p : impl_->fb) {
    p = Color{};
  }
}

bool Strip::Show() { return open_; }  // host: nothing to flush

}  // namespace led
}  // namespace hcp
