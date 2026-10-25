/*
 * hcp::provision — product-level connectivity policy.
 *
 * Owns the "try saved credentials first, fall back to BluFi, show state on the
 * LED, react to the user button" flow. The transition logic itself is the pure
 * Fsm (fsm.h, host-tested); this layer maps its actions onto hcp::net::wifi,
 * hcp::storage, hcp::led, hcp::input and the BluFi module.
 *
 * Copyright (c) 2026 maybetaken
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "hcp/net/wifi.h"
#include "hcp/provision/fsm.h"

namespace hcp {
namespace provision {

// Load stored credentials, bring up the LED + button subsystems and start the
// state machine (connect from storage, or open BluFi when there is nothing to
// try). Wi-Fi must already be initialized by the caller.
void Start();

// Forward a Wi-Fi state change into the machine (from the hcp::net::wifi
// callback).
void NotifyWifiEvent(net::WifiEvent e);

// BluFi just received SSID + passphrase from the phone and will connect on its
// own; the credentials are persisted here and BLE is closed after success.
void NotifyPhoneCredentials(const char* ssid, const char* pass);

// Call immediately before deliberately dropping the link to retry. The driver
// reports a disconnect during CONNECTING as an association failure, so without
// this the self-inflicted drop would consume the retry budget.
void NoteOwnDisconnect();

// True while a link drop should be attributed to our own pre-connect
// Disconnect() rather than to a failed attempt. Callers that report failures to
// the phone must consult it, or a routine teardown looks like a Wi-Fi failure.
bool InOwnDisconnectWindow();

// Report that the service session (MQTT) came up on top of the Wi-Fi link: this
// is what moves the LED from violet-slow (Wi-Fi only) to solid light green.
void NotifyServiceReady();

// Report the opposite: the service session dropped while Wi-Fi stayed up, so the
// LED goes back to violet-slow.
void NotifyServiceLost();

// Forget the stored network and go back to waiting for provisioning - the same
// action as a long press on the button, reached over the network
// (<sn>/control/reconfig). Routed through the state machine on purpose: it owns
// what "no credentials" means for the LED, BLE and the retry counters.
void RequestReset();

/*
 * Home Assistant's light entity for this device: <sn>/control/led with "on"/"off"
 * switches the status light up or blanks it. The state machine keeps running
 * either way - this only silences its output, which is what a request to turn the
 * LED "off" can honestly mean. Off means off, including the alert blinks.
 *
 * It is persisted, because Home Assistant restores the light's state on its side
 * after a restart and an unremembered setting would disagree with the device after
 * every power cycle (the previous firmware stored the same thing as "led_state").
 */
void SetLedEnabled(bool on);
bool LedEnabled();

// Current state, for logging/diagnostics (the MQTT session waits for kOnline).
Fsm::State state();

}  // namespace provision
}  // namespace hcp
