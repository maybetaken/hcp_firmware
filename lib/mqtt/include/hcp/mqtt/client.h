/*
 * hcp::mqtt - MQTT service session on top of the Wi-Fi station link.
 *
 * One broker connection, its reconnect backoff, and the topics the product uses. Session
 * up/down is reported to hcp::provision, which drives the status LED (purple = network up
 * but no session, green = both).
 *
 *   broker host/port    Kconfig default, runtime-overridable by SetBroker() and persisted,
 *                       so SSDP discovery only has to call SetBroker()
 *   credentials         compiled in: the account is created by the user on their own
 *                       broker and is never provisioned over the air
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

// Inbound payloads get their own, larger limit: what the device sends is ours, but a
// Home Assistant config download carries a whole JSON point table (jkbms.json, the widest
// in use, is ~390 bytes) and would be dropped at 256.
#ifndef CONFIG_HCP_MQTT_MAX_INBOUND
#define CONFIG_HCP_MQTT_MAX_INBOUND 512
#endif
constexpr size_t kMaxInboundLen = CONFIG_HCP_MQTT_MAX_INBOUND;

// Device identity: the client id and the prefix of every topic ("<sn>/online",
// "<sn>/cmd", ...), from hcp::identity (storage, else the station MAC) - the same string
// the device announces as its DHCP hostname.
const char* serial_number();

/* Which topic tree a message belongs to.
 *
 * The device has two identities: itself as "<sn>", and its relay outputs as "<sn>-K" - a
 * SIBLING, not a child, so "<sn>/#" does not cover it and it needs its own subscription.
 * The suffix is not ours to choose: the previous firmware used _serial_number + "-K"
 * (main/solar_manager.cpp) and Home Assistant matches on ATTACHED_SUFFIX = "-K"
 * (plugins/Switches.py); changing it silently un-pairs the two. */
enum class Tree : uint8_t {
  kMain = 0,   // "<sn>/..."
  kSwitch,     // "<sn>-K/..."
};

// Learn or change the broker endpoint (dotted IPv4 or hostname). Persisted across
// reboots; an empty host means "not known yet", and the client waits rather than
// retrying forever.
bool SetBroker(const char* host, uint16_t port);

// True once a broker address is known (runtime, storage or Kconfig default).
bool BrokerKnown();

// Start the session thread. Idempotent; returns false if it could not start.
bool Start();

bool IsConnected();

// Publish on the configured state/telemetry topic. Returns false when there is
// no session - the caller does not need to retry, the loop reconnects anyway.
bool Publish(const void* payload, size_t len);
bool PublishTopic(const char* topic, const void* payload, size_t len,
                  Tree tree = Tree::kMain);

// With the MQTT retain flag: the broker keeps the last value, so a client subscribing
// later (HA restart, a debug panel) sees the current state instead of waiting for the
// next event. Used for the availability topic.
bool PublishRetainedTopic(const char* topic, const void* payload, size_t len,
                          Tree tree = Tree::kMain);

// Command topic callback. Runs on the session thread: keep it short, and do not
// block on networking from here.
using CommandHandler = void (*)(const uint8_t* payload, size_t len, void* user);
void SetCommandHandler(CommandHandler cb, void* user = nullptr);

/* Messages on other topics under the device prefix. Home Assistant talks to one device
 * through several (<sn>/config, <sn>/control/cmd, ...), so the client subscribes to
 * "<sn>/#" once and dispatches on the suffix rather than having one command callback.
 *
 * Subscribe() must run before Start(): the subscription list goes out when the session is
 * established. The handler gets a payload copy on the session thread - keep neither the
 * buffer nor the pointer, and move anything slower than a few microseconds off it.
 *
 * Returns false when the route table (CONFIG_HCP_MQTT_MAX_ROUTES) is full or the suffix is
 * too long, so a wiring mistake shows at boot rather than as a topic that never fires. */
using MessageHandler = void (*)(const uint8_t* payload, size_t len, void* user);
constexpr size_t kMaxSuffixLen = 24;
bool Subscribe(const char* suffix, MessageHandler cb, void* user = nullptr,
               Tree tree = Tree::kMain);

/* Announce a second identity on every new session: "<prefix>/online", retained, carrying
 * the JSON document the provider writes.
 *
 * The relay outputs are a device of their own to Home Assistant, which builds their
 * entities from that document (Switches.handle_online reads "switch_count" and grows the
 * device to match), so it must repeat on every connect and hcp::mqtt cannot know the
 * count. There is no last-will for it: Zephyr's client has room for one will topic, and
 * that one keeps the main availability message honest. */
using DocumentFn = int (*)(char* buf, size_t cap);
bool AnnounceDevice(Tree tree, DocumentFn doc);

// Extra fields for the periodic <sn>/diagnostics document (CONFIG_HCP_MQTT_TOPIC_DIAGNOSTICS).
// The provider writes a JSON fragment starting with ',' (e.g. ,"ota":"updating") or returns
// 0 when it has nothing to add, on the session thread: quick and non-blocking. This is how
// another subsystem reports liveness without this module knowing what it reports.
using StateExtraHandler = int (*)(char* buf, size_t cap);
void SetStateExtraHandler(StateExtraHandler cb);

}  // namespace mqtt
}  // namespace hcp
