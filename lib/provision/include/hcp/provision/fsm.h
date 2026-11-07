/*
 * Copyright (c) 2026 maybetaken
 *
 * hcp::provision — provisioning/connectivity state machine (pure logic).
 *
 * Policy lives here so it is host-testable and free of Zephyr/Wi-Fi/LED
 * dependencies: the caller feeds events (boot, Wi-Fi state changes, credentials
 * from the phone, a long-press reset) and executes the returned actions.
 *
 * Shape of the flow the product wants:
 *   boot with saved credentials -> try Wi-Fi (bounded retries), BLE stays off
 *   credentials unusable        -> open BluFi so the phone can re-provision
 *   phone supplied credentials  -> save them; close BLE a moment after success
 *   link lost while online      -> reconnect from saved credentials, then fall
 *                                  back to provisioning
 *   long press                  -> erase credentials, back to provisioning
 *
 * Google C++ Style naming.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <cstdint>

namespace hcp {
namespace provision {

class Fsm {
 public:
  static constexpr int kMaxActions = 4;

  enum class State {
    kIdle,          // not started yet
    kProvisioning,  // BLE open, waiting for the phone
    kWifiTry,       // connecting with credentials, BLE closed
    kOnline,        // Wi-Fi up
  };

  // Steady LED pattern for a state. Light blue blinks while the network link is
  // down (slow = waiting for provisioning, fast = an attempt is running), light
  // violet slow once Wi-Fi is up but the service session (MQTT) is not, light
  // green solid when everything the product needs is connected.
  enum class Pattern { kOff, kBlueSlow, kBlueFast, kVioletSlow, kGreenSolid };

  // One-shot blink sequence layered on top of the steady pattern. The names say
  // what happened, not what colour it is - the executor picks hues, and renders
  // kErased3 as white so it stays visible on top of a violet or blue steady
  // pattern.
  enum class Alert { kNone, kFailed3, kErased3 };

  enum class Action {
    kNone,
    kEnableBle,        // blufi_start()
    kDisableBle,       // blufi_stop()
    kCloseBleDelayed,  // blufi_stop() after the grace period (let the phone get
                       // the final report first - mini-programs are slow)
    kConnectWifi,      // connect using the credentials we hold
    kSaveCreds,        // persist SSID/passphrase
    kEraseCreds,       // remove them
  };

  struct Output {
    State state = State::kIdle;
    Pattern pattern = Pattern::kOff;
    Alert alert = Alert::kNone;
    Action actions[kMaxActions] = {Action::kNone};
    int num_actions = 0;
  };

  explicit Fsm(int wifi_attempts = 3) : wifi_attempts_(wifi_attempts) {}

  // Entry point. `have_creds` = stored credentials exist and are worth trying.
  Output Start(bool have_creds);

  Output OnPhoneCreds();          // the phone just supplied SSID+pass and is
                                  // driving the connect itself
  Output OnWifiConnected();
  Output OnWifiConnectFailed();   // one attempt failed
  // A disconnect notification. Only meaningful while online (link lost ->
  // reconnect from storage). While an attempt is in flight it is almost always
  // our own pre-connect Disconnect(), so it must not consume the retry budget.
  Output OnWifiDisconnected();
  // Backstop for the case where the driver reports nothing at all: the ESP32
  // driver raises no CONNECT_RESULT for association failures that happen after
  // the connect call returned, so an attempt that never completes is failed by
  // its deadline - otherwise the machine would blink in kWifiTry forever with
  // BLE closed and the device would be unreachable.
  Output OnAttemptTimeout();
  Output OnResetRequested();      // long press: forget credentials

  /* The slow background re-attempt of the stored network, fired by the caller while
   * the machine is waiting for a phone. Reaching kProvisioning is not evidence that
   * the credentials are wrong - the common reason is a network that was still booting
   * when the (much shorter) attempt budget ran out - so without this the box keeps
   * advertising BLE next to an access point that came back minutes ago.
   *
   * It is a fresh round of ordinary attempts rather than a special state: kWifiTry,
   * the same per-attempt deadline behind it, the same fall-back in front of it. It
   * does nothing when there are no credentials to retry or when the machine is not
   * waiting for a phone.
   */
  Output OnNetworkRetry();

  // The service session (MQTT) came up on top of the Wi-Fi link: this is what
  // turns the LED from violet to green. Ignored unless we are online.
  Output OnServiceReady();

  // The service session dropped while Wi-Fi stayed up: go back to waiting
  // (violet), which is also what tells the user the broker is unreachable.
  Output OnServiceLost();

  State state() const { return state_; }
  Pattern pattern() const { return pattern_; }
  int attempts() const { return attempts_; }
  bool ble_open() const { return ble_open_; }

 private:
  Output Make(State s, Pattern p, Alert a);
  Output NoChange() const;  // state/pattern only, no actions
  static void Push(Output& o, Action act);

  State state_ = State::kIdle;
  Pattern pattern_ = Pattern::kOff;
  int attempts_ = 0;
  int wifi_attempts_;
  bool ble_open_ = false;
  // Stored credentials exist, so a network retry has something to connect to.
  bool have_creds_ = false;
  // Wi-Fi is up *and* the service session (MQTT) is established.
  bool service_up_ = false;
  // Credentials came from the phone in this session: on success, shut BLE down
  // (delayed) and persist; on failure, stay reachable over BLE.
  bool from_phone_ = false;
};

}  // namespace provision
}  // namespace hcp
