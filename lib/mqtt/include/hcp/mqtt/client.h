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

// Inbound payloads get their own, larger limit: what the device sends is ours,
// but a Home Assistant config download carries a whole JSON point table (the
// widest in use, jkbms.json, is about 390 bytes) and would be dropped at 256.
#ifndef CONFIG_HCP_MQTT_MAX_INBOUND
#define CONFIG_HCP_MQTT_MAX_INBOUND 512
#endif
constexpr size_t kMaxInboundLen = CONFIG_HCP_MQTT_MAX_INBOUND;

// Device identity, used as the client id and as the topic prefix: every topic is
// "<sn>/<suffix>" ("<sn>/online", "<sn>/cmd", ...), like the reference firmware, so
// the broker/HA side can address one device out of many. The number itself comes
// from hcp::identity (storage, else the station MAC) - the same string the device
// announces as its DHCP hostname.
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

/*
 * Messages on other topics under the device prefix. Home Assistant talks to one
 * device through several of them (<sn>/config, <sn>/control/cmd, <sn>/led ...), so
 * a single command callback is not enough: the client subscribes to "<sn>/#" once
 * and dispatches on the suffix.
 *
 * Subscribe() must be called before Start() - the subscription list is sent when
 * the session is established. The handler runs on the session thread with a copy
 * of the payload, so it may keep neither the buffer nor the pointer; anything that
 * takes longer than a few microseconds belongs on the caller's own queue.
 *
 * Returns false when the route table is full (CONFIG_HCP_MQTT_MAX_ROUTES) or the
 * suffix is too long, so a wiring mistake shows up at boot instead of as a topic
 * that never fires.
 */
using MessageHandler = void (*)(const uint8_t* payload, size_t len, void* user);
constexpr size_t kMaxSuffixLen = 24;
bool Subscribe(const char* suffix, MessageHandler cb, void* user = nullptr);

// Extra fields for the periodic <sn>/state document. The provider writes a JSON
// fragment that starts with ',' (e.g. ,"ota":"updating") or returns 0 when it has
// nothing to report; it runs on the session thread, so it must be quick and
// non-blocking. This is how another subsystem puts its liveness on the same topic
// without this module having to know what it reports.
using StateExtraHandler = int (*)(char* buf, size_t cap);
void SetStateExtraHandler(StateExtraHandler cb);

}  // namespace mqtt
}  // namespace hcp
