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

using WifiCallback = void (*)(WifiEvent evt, void* user);

namespace wifi {

// Register a state callback (optional). Safe to call before Init.
void SetCallback(WifiCallback cb, void* user = nullptr);

// Bring up the Wi-Fi interface (driver + mgmt events). Idempotent.
bool Init();

// Connect as a station (async). Progress/result via the callback.
// Returns false on immediate validation error, true if a connect was issued.
bool Connect(const WifiCredential& cred);

bool Disconnect();
bool IsConnected();

}  // namespace wifi
}  // namespace net
}  // namespace hcp
