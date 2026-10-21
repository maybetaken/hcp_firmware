/*
 * HCP networking abstraction — Wi-Fi station (connect to an AP, get an IP).
 * Backend-agnostic façade over the Zephyr Wi-Fi management API. On host there
 * is no backend yet (net is target-only); the interface is kept small so app
 * core / BluFi provisioning can drive it uniformly. Google C++ Style naming.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <cstdint>

namespace hcp {
namespace net {

struct WifiCredential {
  const char* ssid;
  const char* psk;  // nullptr or "" => open network
};

// Connection state reported to the callback.
enum class WifiEvent {
  kConnecting,
  kConnected,      // associated + IPv4 acquired
  kDisconnected,   // link lost after being up
  kConnectFailed,  // association/authentication failed
};

// A Wi-Fi scan result (subset, enough for BluFi AP list + conn report).
struct ScanResult {
  char ssid[33];
  int8_t rssi;
  uint8_t bssid[6];  // AP MAC (for the BluFi conn report extra info)
  uint8_t channel;
};

using WifiCallback = void (*)(WifiEvent evt, void* user);
// Scan callback: invoked once per AP, then once with count==0 to signal done.
using ScanCallback = void (*)(const ScanResult* result, void* user);

namespace wifi {

// Register a state callback (optional). Safe to call before Init.
void SetCallback(WifiCallback cb, void* user = nullptr);

// Bring up the Wi-Fi interface (driver + mgmt events). Idempotent.
bool Init();

// Connect as a station (async). Progress/result via the callback.
// Returns false on immediate validation error, true if a connect was issued.
bool Connect(const WifiCredential& cred);

// Start an async Wi-Fi scan; each AP is delivered to the callback, and a final
// call with result==nullptr marks completion. Returns false on error.
bool Scan(ScanCallback cb, void* user = nullptr);

bool Disconnect();
bool IsConnected();

// True when the driver already holds a link or is in the middle of one
// (connecting / associated / handshaking). Callers use it to decide whether a
// Disconnect() - plus waiting for its event - is needed at all: dropping a link
// that never existed just fails inside the driver and the caller ends up burning
// its whole timeout for nothing.
bool LinkActive();

}  // namespace wifi
}  // namespace net
}  // namespace hcp
