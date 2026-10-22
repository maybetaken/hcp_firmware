/*
 * hcp::mqtt - Zephyr backend: one MQTT session on its own thread.
 *
 * Zephyr's MQTT library is not self-driving: the application must poll the
 * socket and feed mqtt_input(), so this runs a single thread that
 *   waits for hcp::provision to be online and for a broker address,
 *   connects, subscribes to the command topic, pumps traffic,
 *   publishes whatever the application queued,
 *   and on any error tears the client down and retries with a capped backoff.
 *
 * Publish() from other threads only touches a message queue, never the client,
 * because the library is not thread-safe.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifdef CONFIG_HCP_MQTT

#include "hcp/mqtt/client.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/socket.h>

#include "hcp/log/log.h"
#include "hcp/mqtt/backoff.h"
#include "hcp/net/wifi.h"
#include "hcp/provision/provision.h"
#include "hcp/storage/store.h"

namespace hcp {
namespace mqtt {
namespace {

/* This module logs through hcp::log: the application owns the log backend and the
 * per-domain levels, and this translation unit registers no log module of its
 * own. */
#define MQTT_LOGI(...) HCP_LOGI(hcp::log::kDomainMqtt, __VA_ARGS__)
#define MQTT_LOGW(...) HCP_LOGW(hcp::log::kDomainMqtt, __VA_ARGS__)
#define MQTT_LOGD(...) HCP_LOGD(hcp::log::kDomainMqtt, __VA_ARGS__)

constexpr char kKeyHost[] = "svc/host";
constexpr char kKeyPort[] = "svc/port";
constexpr char kKeySnr[] = "dev/snr";
constexpr size_t kHostMax = 64;
constexpr int kTxQueueDepth = 4;
constexpr int kPollMs = 500;

struct TxItem {
  char topic[kMaxTopicLen];  // fully prefixed ("<sn>/..."), built at enqueue time
  uint16_t len;
  uint8_t retain;
  uint8_t payload[kMaxPayloadLen];
};

uint8_t rx_buffer[CONFIG_HCP_MQTT_BUF_SIZE];
uint8_t tx_buffer[CONFIG_HCP_MQTT_BUF_SIZE];
struct mqtt_client cli;
struct sockaddr_in broker_addr;

char host[kHostMax];
char sn[kMaxTopicLen];
uint16_t port;
uint16_t message_id = 1;
volatile bool connected;
volatile bool started;

storage::Store store;
CommandHandler cmd_cb;
void* cmd_user;
bool waiting_for_broker_logged;  // so the poll loop does not spam the console
Backoff backoff(CONFIG_HCP_MQTT_RETRY_BASE_MS, CONFIG_HCP_MQTT_RETRY_CAP_MS);

K_MSGQ_DEFINE(tx_queue, sizeof(TxItem), kTxQueueDepth, 4);
K_THREAD_STACK_DEFINE(session_stack, CONFIG_HCP_MQTT_STACK_SIZE);
struct k_thread session_thread;

mqtt_utf8 Utf8(const char* s) {
  mqtt_utf8 u;
  u.utf8 = reinterpret_cast<const uint8_t*>(s);
  u.size = static_cast<uint32_t>(strlen(s));
  return u;
}

/* Identity: a stored serial number wins, otherwise derive one from the station
 * MAC and keep it, so broker-side topics stay stable across reflashes. */
void EnsureSerialNumber() {
  if (sn[0] != '\0') {
    return;
  }
  if (store.GetString(kKeySnr, sn, sizeof(sn)) && sn[0] != '\0') {
    return;
  }
  uint8_t mac[6];
  if (net::wifi::GetHwAddr(mac)) {
    snprintk(sn, sizeof(sn), "hcp-%02x%02x%02x%02x%02x%02x", mac[0], mac[1],
             mac[2], mac[3], mac[4], mac[5]);
    store.SetString(kKeySnr, sn);
    MQTT_LOGI("device identity %s", sn);
    return;
  }
  // Interface not ready; use a placeholder and try to derive again next boot.
  strncpy(sn, "hcp-pending", sizeof(sn) - 1);
  sn[sizeof(sn) - 1] = '\0';
}

/* Every topic is "<sn>/<suffix>", like the reference firmware. */
bool FullTopic(const char* suffix, char* out, size_t cap) {
  if (sn[0] == '\0' || suffix == nullptr) {
    return false;
  }
  const int n = snprintk(out, cap, "%s/%s", sn, suffix);
  return n > 0 && static_cast<size_t>(n) < cap;
}

/* Broker address: storage (learned at runtime) wins, then the build-time
 * default. Returns false when neither is set, i.e. we must wait for discovery. */
void LoadBroker() {
  host[0] = '\0';
  port = CONFIG_HCP_MQTT_PORT;
  if (store.GetString(kKeyHost, host, sizeof(host)) && host[0] != '\0') {
    int32_t stored_port = 0;
    if (store.GetInt(kKeyPort, stored_port) && stored_port > 0 &&
        stored_port <= 65535) {
      port = static_cast<uint16_t>(stored_port);
    }
    return;
  }
  strncpy(host, CONFIG_HCP_MQTT_HOST, sizeof(host) - 1);
  host[sizeof(host) - 1] = '\0';
}

/* Dotted IPv4 fast path, otherwise resolve by name. Returns false on failure. */
bool ResolveBroker() {
  memset(&broker_addr, 0, sizeof(broker_addr));
  broker_addr.sin_family = AF_INET;
  broker_addr.sin_port = htons(port);
  if (zsock_inet_pton(AF_INET, host, &broker_addr.sin_addr) == 1) {
    return true;
  }

  struct zsock_addrinfo hints = {};
  struct zsock_addrinfo* res = nullptr;
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  char svc[8];
  snprintk(svc, sizeof(svc), "%u", static_cast<unsigned>(port));
  int rc = zsock_getaddrinfo(host, svc, &hints, &res);
  if (rc != 0 || res == nullptr) {
    MQTT_LOGW("cannot resolve broker %s (rc=%d)", host, rc);
    if (res != nullptr) {
      zsock_freeaddrinfo(res);
    }
    return false;
  }
  memcpy(&broker_addr, res->ai_addr, sizeof(broker_addr));
  broker_addr.sin_port = htons(port);
  zsock_freeaddrinfo(res);
  return true;
}

void Subscribe() {
  static mqtt_topic cmd_topic;
  static char cmd_name[kMaxTopicLen];
  static mqtt_subscription_list list;
  if (!FullTopic(CONFIG_HCP_MQTT_TOPIC_CMD, cmd_name, sizeof(cmd_name))) {
    MQTT_LOGW("command topic too long");
    return;
  }
  cmd_topic.topic = Utf8(cmd_name);
  cmd_topic.qos = MQTT_QOS_0_AT_MOST_ONCE;
  list.list = &cmd_topic;
  list.list_count = 1;
  list.message_id = message_id++;
  int rc = mqtt_subscribe(&cli, &list);
  if (rc != 0) {
    MQTT_LOGW("subscribe failed (%d)", rc);
  }
}

/* Announce ourselves once per session. Retained, and paired with the last-will
 * message below, so the broker always knows whether this device is up - even if
 * we vanish without a clean disconnect. */
void PublishOnline() {
  char topic[kMaxTopicLen];
  if (!FullTopic(CONFIG_HCP_MQTT_TOPIC_ONLINE, topic, sizeof(topic))) {
    return;
  }
  const mqtt_publish_param param = {};
  mqtt_publish_param p = param;
  p.message.topic.topic = Utf8(topic);
  p.message.topic.qos = MQTT_QOS_0_AT_MOST_ONCE;
  p.message.payload.data = reinterpret_cast<uint8_t*>(
      const_cast<char*>(CONFIG_HCP_MQTT_FW_VERSION));
  p.message.payload.len = sizeof(CONFIG_HCP_MQTT_FW_VERSION) - 1;
  p.retain_flag = 1U;
  p.message_id = message_id++;
  if (mqtt_publish(&cli, &p) != 0) {
    MQTT_LOGW("online announcement failed");
  }
}

/* Liveness signal for the broker/Home Assistant: without a periodic message the
 * device is silent between events and a debug subscriber sees nothing. */
void PublishStateHeartbeat() {
  static char payload[64];
  const int n = snprintk(payload, sizeof(payload), "{\"up\":%u,\"fw\":\"%s\"}",
                          static_cast<unsigned>(k_uptime_get() / 1000),
                          CONFIG_HCP_MQTT_FW_VERSION);
  if (n > 0) {
    PublishTopic(CONFIG_HCP_MQTT_TOPIC_STATE, payload, static_cast<size_t>(n));
  }
}

/* Queue a publish for the session thread; only that thread may touch the client. */
bool Enqueue(const char* topic, const void* payload, size_t len, bool retain) {
  if (topic == nullptr || payload == nullptr || len == 0 ||
      len > kMaxPayloadLen || !connected) {
    return false;
  }
  TxItem item;
  if (!FullTopic(topic, item.topic, sizeof(item.topic))) {
    MQTT_LOGW("topic too long: %s", topic);
    return false;
  }
  item.len = static_cast<uint16_t>(len);
  item.retain = retain ? 1U : 0U;
  memcpy(item.payload, payload, len);
  /* Drop when the queue is full instead of blocking: callers are the collection
   * loop and command handlers, not the MQTT path itself. */
  return k_msgq_put(&tx_queue, &item, K_NO_WAIT) == 0;
}

void HandleInbound(const mqtt_publish_param& pub) {
  uint32_t len = pub.message.payload.len;
  if (len > kMaxPayloadLen) {
    MQTT_LOGW("dropping inbound payload of %u bytes", len);
    return;
  }
  uint8_t buf[kMaxPayloadLen];
  int rc = mqtt_readall_publish_payload(&cli, buf, len);
  if (rc != 0) {
    MQTT_LOGW("payload read failed (%d)", rc);
    return;
  }
  MQTT_LOGD("%u bytes on %s", (unsigned)len, CONFIG_HCP_MQTT_TOPIC_CMD);
  if (cmd_cb != nullptr) {
    cmd_cb(buf, len, cmd_user);
  }
}

/* Sends everything queued by other threads. Only called from the session thread,
 * which is the only one allowed to touch the client. */
void DrainTxQueue() {
  TxItem item;
  while (k_msgq_get(&tx_queue, &item, K_NO_WAIT) == 0) {
    const mqtt_publish_param param = {};
    mqtt_publish_param p = param;
    p.message.topic.topic = Utf8(item.topic);
    p.message.topic.qos = MQTT_QOS_0_AT_MOST_ONCE;
    p.message.payload.data = item.payload;
    p.message.payload.len = item.len;
    p.retain_flag = item.retain ? 1U : 0U;
    p.message_id = message_id++;
    int rc = mqtt_publish(&cli, &p);
    if (rc != 0) {
      MQTT_LOGW("publish to %s failed (%d)", item.topic, rc);
      return;
    }
  }
}

void OnEvent(struct mqtt_client* /*client*/, const struct mqtt_evt* evt) {
  switch (evt->type) {
    case MQTT_EVT_CONNACK:
      if (evt->result == 0) {
        connected = true;
        backoff.Reset();
        MQTT_LOGI("session up with %s:%u", host, static_cast<unsigned>(port));
        Subscribe();
        PublishOnline();
        provision::NotifyServiceReady();
      } else {
        /* A rejected CONNACK is usually a wrong account or a client-id clash -
         * worth being loud about, the user has to fix it on the broker. */
        MQTT_LOGW("broker refused the session (return code %d)",
                  static_cast<int>(evt->result));
        connected = false;
      }
      break;

    case MQTT_EVT_DISCONNECT:
      if (connected) {
        MQTT_LOGW("broker dropped the session");
        connected = false;
        provision::NotifyServiceLost();
      }
      break;

    case MQTT_EVT_PUBLISH:
      HandleInbound(evt->param.publish);
      break;

    default:
      break;
  }
}

void InitClient() {
  mqtt_client_init(&cli);
  static mqtt_utf8 user;
  static mqtt_utf8 pass;
  user = Utf8(CONFIG_HCP_MQTT_USER);
  pass = Utf8(CONFIG_HCP_MQTT_PASS);

  /* Last will: if the link dies without a DISCONNECT, the broker publishes
   * "offline" on the same retained availability topic. */
  static mqtt_topic will_topic;
  static mqtt_utf8 will_message;
  static char will_name[kMaxTopicLen];
  if (FullTopic(CONFIG_HCP_MQTT_TOPIC_ONLINE, will_name, sizeof(will_name))) {
    will_topic.topic = Utf8(will_name);
    will_topic.qos = MQTT_QOS_0_AT_MOST_ONCE;
    will_message = Utf8("offline");
    cli.will_topic = &will_topic;
    cli.will_message = &will_message;
    cli.will_retain = 1U;
  }

  cli.broker = &broker_addr;
  cli.evt_cb = OnEvent;
  cli.client_id = Utf8(sn);
  cli.user_name = user.size > 0 ? &user : nullptr;
  cli.password = pass.size > 0 ? &pass : nullptr;
  cli.keepalive = CONFIG_HCP_MQTT_KEEPALIVE_SEC;
  cli.clean_session = 1U;
#if defined(CONFIG_MQTT_VERSION_5_0)
  cli.protocol_version = MQTT_VERSION_5_0;
#else
  cli.protocol_version = MQTT_VERSION_3_1_1;
#endif
  cli.rx_buf = rx_buffer;
  cli.rx_buf_size = sizeof(rx_buffer);
  cli.tx_buf = tx_buffer;
  cli.tx_buf_size = sizeof(tx_buffer);
  cli.transport.type = MQTT_TRANSPORT_NON_SECURE;
}

bool PollReadable(int timeout_ms) {
  struct zsock_pollfd fds[1];
  fds[0].fd = cli.transport.tcp.sock;
  fds[0].events = ZSOCK_POLLIN;
  int rc = zsock_poll(fds, 1, timeout_ms);
  if (rc < 0) {
    MQTT_LOGW("poll failed (%d)", errno);
    return false;
  }
  return rc > 0 && (fds[0].revents & ZSOCK_POLLIN) != 0;
}

/* Waits for CONNACK, which arrives while pumping input. */
bool WaitConnack() {
  int64_t deadline = k_uptime_get() + CONFIG_HCP_MQTT_CONNECT_TIMEOUT_MS;
  while (started && !connected && k_uptime_get() < deadline) {
    if (!PollReadable(kPollMs)) {
      if (!started) {
        return false;
      }
      continue;
    }
    if (mqtt_input(&cli) < 0) {
      return false;
    }
  }
  return connected;
}

/* One read/keepalive round. Returns false when the connection is gone. */
bool Pump() {
  if (PollReadable(kPollMs) && mqtt_input(&cli) < 0) {
    return false;
  }
  if (connected) {
    DrainTxQueue();
#if CONFIG_HCP_MQTT_STATE_INTERVAL_SEC > 0
    static int64_t last_state_ms;
    const int64_t now = k_uptime_get();
    if (now - last_state_ms >= CONFIG_HCP_MQTT_STATE_INTERVAL_SEC * 1000) {
      last_state_ms = now;
      PublishStateHeartbeat();
    }
#endif
  }
  int rc = mqtt_live(&cli);
  return rc == 0 || rc == -EAGAIN;
}

void ConnectAndServe() {
  if (!ResolveBroker()) {
    return;
  }
  InitClient();
  connected = false;
  if (mqtt_connect(&cli) != 0) {
    const int err = errno;
    MQTT_LOGW("connect request to %s:%u failed (errno %d)", host,
              static_cast<unsigned>(port), err);
    mqtt_abort(&cli);
    return;
  }
  if (!WaitConnack()) {
    MQTT_LOGW("no CONNACK from %s:%u within %d ms", host,
              static_cast<unsigned>(port), CONFIG_HCP_MQTT_CONNECT_TIMEOUT_MS);
    mqtt_abort(&cli);
    return;
  }

  while (started && connected) {
    if (!Pump()) {
      break;
    }
  }

  if (connected) {
    connected = false;
    provision::NotifyServiceLost();
  }
  mqtt_abort(&cli);
}

void SessionThread(void*, void*, void*) {
  while (true) {
    LoadBroker();
    if (!BrokerKnown()) {
      if (!waiting_for_broker_logged) {
        MQTT_LOGI("waiting for a broker address (SSDP or CONFIG_HCP_MQTT_HOST)");
        waiting_for_broker_logged = true;
      }
      k_sleep(K_SECONDS(5));
      continue;
    }
    if (waiting_for_broker_logged) {
      MQTT_LOGI("broker address known: %s:%u", host, static_cast<unsigned>(port));
      waiting_for_broker_logged = false;
    }
    if (provision::state() != provision::Fsm::State::kOnline) {
      k_sleep(K_SECONDS(1));
      continue;
    }
    ConnectAndServe();
    uint32_t wait_ms = backoff.NextMs();
    if (wait_ms > 0) {
      k_sleep(K_MSEC(wait_ms));
    }
  }
}

}  // namespace

bool BrokerKnown() { return host[0] != '\0'; }

bool SetBroker(const char* new_host, uint16_t new_port) {
  if (new_host == nullptr || new_host[0] == '\0') {
    return false;
  }
  if (strlen(new_host) >= sizeof(host)) {
    return false;
  }
  strncpy(host, new_host, sizeof(host) - 1);
  host[sizeof(host) - 1] = '\0';
  if (new_port != 0) {
    port = new_port;
  }
  bool ok = store.SetString(kKeyHost, host) &&
            store.SetInt(kKeyPort, static_cast<int32_t>(port));
  MQTT_LOGI("broker set to %s:%u (%s)", host, static_cast<unsigned>(port),
            ok ? "persisted" : "session only");
  return true;
}

bool Start() {
  if (started) {
    return true;
  }
  if (!store.Open()) {
    MQTT_LOGW("storage unavailable, broker address will not persist");
  }
  EnsureSerialNumber();
  LoadBroker();
  started = true;
  k_thread_create(&session_thread, session_stack,
                  K_THREAD_STACK_SIZEOF(session_stack), SessionThread, nullptr,
                  nullptr, nullptr, K_PRIO_PREEMPT(10), 0, K_NO_WAIT);
  k_thread_name_set(&session_thread, "mqtt");
  return true;
}

bool IsConnected() { return connected; }

const char* serial_number() { return sn; }

bool PublishTopic(const char* topic, const void* payload, size_t len) {
  return Enqueue(topic, payload, len, false);
}

bool PublishRetainedTopic(const char* topic, const void* payload, size_t len) {
  return Enqueue(topic, payload, len, true);
}

bool Publish(const void* payload, size_t len) {
  return PublishTopic(CONFIG_HCP_MQTT_TOPIC_STATE, payload, len);
}

void SetCommandHandler(CommandHandler cb, void* user) {
  cmd_cb = cb;
  cmd_user = user;
}

}  // namespace mqtt
}  // namespace hcp

#endif /* CONFIG_HCP_MQTT */
