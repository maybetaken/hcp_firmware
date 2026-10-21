/*
 * hcp::provision — product-level connectivity policy.
 *
 * Owns the "try saved credentials first, fall back to BluFi, show state on the
 * LED, react to the user button" flow. The transition logic itself is the pure
 * Fsm (fsm.h, host-tested); this layer maps its actions onto hcp::net::wifi,
 * hcp::storage, hcp::led, hcp::input and the BluFi module.
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

// Current state, for logging/diagnostics.
Fsm::State state();

}  // namespace provision
}  // namespace hcp
