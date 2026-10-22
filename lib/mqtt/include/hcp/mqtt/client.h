/*
 * hcp::mqtt - MQTT service session on top of the Wi-Fi station link.
 *
 * Deliberately small: the module owns one broker connection, its reconnect
 * backoff and the two topics the product uses. It reports the session up/down to
 * hcp::provision, which is what drives the status LED (purple while the network
 * is up but this session is not, green when both are).
 *
 * Where the pieces come from:
 *   broker host/port - compiled-in default (Kconfig), overridable at runtime by
 *     SetBroker() and persisted in hcp::storage, so the discovery layer (SSDP)
 *     only has to call SetBroker() when it learns the address;
 *   username/password - compiled in on purpose: the account is created by the
 *     user on their own broker and is not provisioned over the air.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace hcp {
namespace mqtt {

// Topic sizes accepted by Publish()/the command handler.
constexpr size_t kMaxTopicLen = 64;
constexpr size_t kMaxPayloadLen = 256;

// Device identity, used as the client id and as the topic prefix: every topic is
// "<sn>/<suffix>" ("<sn>/online", "<sn>/cmd", ...), like the reference firmware, so
// the broker/HA side can address one device out of many. Derived from the station
// MAC on first boot and then kept in storage.
const char* serial_number();

// Learn or change the broker endpoint (host is a dotted-IPv4 string or a
// hostname). Persisted so a reboot keeps it; empty host means "not known yet"
// and the client keeps waiting instead of retrying forever.
bool SetBroker(const char* host, uint16_t port);

// True once a broker address is known (runtime, storage or Kconfig default).
bool BrokerKnown();

// Start the session thread. Idempotent; returns false if it could not start.
bool Start();

bool IsConnected();

// Publish on the configured state/telemetry topic. Returns false when there is
// no session - the caller does not need to retry, the loop reconnects anyway.
bool Publish(const void* payload, size_t len);
bool PublishTopic(const char* topic, const void* payload, size_t len);

// Same, but with the MQTT retain flag: the broker keeps the last value, so a
// client that subscribes later (Home Assistant restart, a debug panel hitting
// "listen") sees the current state immediately instead of waiting for the next
// event. Used for the availability topic.
bool PublishRetainedTopic(const char* topic, const void* payload, size_t len);

// Command topic callback. Runs on the session thread: keep it short, and do not
// block on networking from here.
using CommandHandler = void (*)(const uint8_t* payload, size_t len, void* user);
void SetCommandHandler(CommandHandler cb, void* user = nullptr);

}  // namespace mqtt
}  // namespace hcp
