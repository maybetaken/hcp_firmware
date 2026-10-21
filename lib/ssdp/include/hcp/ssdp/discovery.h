/*
 * hcp::ssdp - Home Assistant announcement listener.
 *
 * The HA side (custom component solar_manager/ssdp.py) broadcasts every 5 s:
 *
 *   UDP -> 239.255.255.250:1900   payload: "maybetaken: mqtt://<ha-ip>"
 *
 * Parsing is kept pure and host-tested; the socket side is a single listener
 * thread. When an announcement is accepted, the broker address is handed to
 * hcp::mqtt (and persisted there), so nothing else has to be configured.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace hcp {
namespace ssdp {

constexpr uint16_t kSsdpPort = 1900;
constexpr char kSsdpGroup[] = "239.255.255.250";
constexpr char kMarker[] = "maybetaken:";
constexpr size_t kUrlMax = 64;

// True when the datagram carries our marker; copies whatever follows it, trimmed
// of surrounding blanks/CR/LF, into url_out. Mirrors SSDPListener::parseHAAddress.
bool ParseAnnouncement(const char* data, size_t len, char* url_out, size_t url_cap);

// "mqtt://192.168.31.20" / "mqtt://host:1883" -> host + port (default 1883).
// Returns false when the string is not an mqtt:// URL.
bool ParseMqttUrl(const char* url, char* host, size_t host_cap, uint16_t* port);

// Start the listener thread (idempotent). Returns false if it could not start.
bool Start();

// Last URL accepted, or "" while discovery has not produced one yet.
const char* last_url();

}  // namespace ssdp
}  // namespace hcp
