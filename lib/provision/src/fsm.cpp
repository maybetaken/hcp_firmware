/*
 * hcp::provision — Fsm transition logic (pure, host-tested).
 * SPDX-License-Identifier: Apache-2.0
 */
#include "hcp/provision/fsm.h"

namespace hcp {
namespace provision {

void Fsm::Push(Output& o, Action act) {
  if (act == Action::kNone || o.num_actions >= kMaxActions) {
    return;
  }
  o.actions[o.num_actions++] = act;
}

Fsm::Output Fsm::Make(State s, Pattern p, Alert a) {
  state_ = s;
  pattern_ = p;
  Output o;
  o.state = s;
  o.pattern = p;
  o.alert = a;
  return o;
}

Fsm::Output Fsm::Start(bool have_creds) {
  from_phone_ = false;
  if (have_creds) {
    // Try the network first; the radio stays off until we know we need it.
    ble_open_ = false;
    attempts_ = 1;
    Output o = Make(State::kWifiTry, Pattern::kBlueFast, Alert::kNone);
    Push(o, Action::kConnectWifi);
    return o;
  }
  ble_open_ = true;
  attempts_ = 0;
  Output o = Make(State::kProvisioning, Pattern::kBlueSlow, Alert::kNone);
  Push(o, Action::kEnableBle);
  return o;
}

Fsm::Output Fsm::OnPhoneCreds() {
  // The phone drives the connect (and reports the result over BLE), so we only
  // persist here - no kConnectWifi, and BLE must stay open for the exchange.
  from_phone_ = true;
  attempts_ = 1;
  ble_open_ = true;
  Output o = Make(State::kWifiTry, Pattern::kBlueFast, Alert::kNone);
  Push(o, Action::kSaveCreds);
  return o;
}

Fsm::Output Fsm::OnWifiConnected() {
  attempts_ = 0;
  Output o = Make(State::kOnline, Pattern::kGreenSolid, Alert::kNone);
  if (from_phone_) {
    // Give the mini-program a grace window to receive the final SUCCESS report
    // before the link and the radio go away (it never re-polls after that).
    from_phone_ = false;
    ble_open_ = false;
    Push(o, Action::kCloseBleDelayed);
  }
  return o;
}

Fsm::Output Fsm::OnWifiConnectFailed() {
  if (state_ != State::kWifiTry) {
    Output o;
    o.state = state_;
    o.pattern = pattern_;
    return o;
  }

  if (from_phone_) {
    // Stay reachable: the phone is still there and can retry with other
    // credentials. The BluFi layer already sent the FAIL conn report.
    from_phone_ = false;
    return Make(State::kProvisioning, Pattern::kBlueSlow, Alert::kRed3);
  }

  attempts_++;
  if (attempts_ < wifi_attempts_) {
    Output o = Make(State::kWifiTry, Pattern::kBlueFast, Alert::kNone);
    Push(o, Action::kConnectWifi);
    return o;
  }

  // Saved credentials are not working (wrong pass, AP gone, router rebooted
  // with new settings...): fall back to provisioning instead of retrying
  // forever.
  attempts_ = 0;
  ble_open_ = true;
  Output o = Make(State::kProvisioning, Pattern::kBlueSlow, Alert::kRed3);
  Push(o, Action::kEnableBle);
  return o;
}

Fsm::Output Fsm::OnWifiLost() {
  if (state_ != State::kOnline) {
    Output o;
    o.state = state_;
    o.pattern = pattern_;
    return o;
  }
  attempts_ = 1;
  Output o = Make(State::kWifiTry, Pattern::kBlueFast, Alert::kNone);
  Push(o, Action::kConnectWifi);
  return o;
}

Fsm::Output Fsm::OnResetRequested() {
  from_phone_ = false;
  attempts_ = 0;
  Output o = Make(State::kProvisioning, Pattern::kBlueSlow, Alert::kPurple3);
  Push(o, Action::kEraseCreds);
  if (!ble_open_) {
    ble_open_ = true;
    Push(o, Action::kEnableBle);
  }
  return o;
}

}  // namespace provision
}  // namespace hcp
