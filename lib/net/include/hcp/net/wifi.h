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
  kConnected,   // associated + IPv4 acquired
  kDisconnected,
};

// A Wi-Fi scan result (subset, enough for BluFi AP list).
struct ScanResult {
  char ssid[33];
  int8_t rssi;
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

}  // namespace wifi
}  // namespace net
}  // namespace hcp
