/*
 * hcp::collector — Zephyr side: the queue, the worker thread and the MQTT routes.
 * See collector.h for why there is exactly one thread allowed to touch the bus.
 *
 * Copyright (c) 2026 maybetaken
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "hcp/collector/collector.h"

#ifdef CONFIG_HCP_COLLECTOR

#include <string.h>

#include <zephyr/kernel.h>

#include "hcp/collector/schedule.h"
#include "hcp/kernel/kernel.h"
#include "hcp/log/log.h"
#include "hcp/modbus/master.h"
#include "hcp/mqtt/client.h"
#ifdef CONFIG_HCP_OTA
#include "hcp/ota/ota.h"
#endif
#ifdef CONFIG_HCP_PROVISION
#include "hcp/provision/provision.h"
#endif
#ifdef CONFIG_HCP_ENABLE_SWITCH_CONTROL
#include "hcp/switch/command.h"
#include "hcp/switch/relay.h"
#endif
#include "hcp/registry/storage_kv.h"
#include "hcp/registry/wire.h"

#define COL_LOGI(...) HCP_LOGI(hcp::log::kDomainModbus, __VA_ARGS__)
#define COL_LOGW(...) HCP_LOGW(hcp::log::kDomainModbus, __VA_ARGS__)
#define COL_LOGD(...) HCP_LOGD(hcp::log::kDomainModbus, __VA_ARGS__)

namespace hcp {
namespace collector {
namespace {

/* One payload per queue entry, copied out of the MQTT session thread's buffer.
 * Fixed size from a slab, on purpose: the version this replaces put raw malloc'd
 * pointers in the queue and freed them per case in the consumer. */
struct Slot {
  Command cmd;
  uint16_t len;
  uint8_t data[CONFIG_HCP_COLLECTOR_MAX_MSG];
};

K_MEM_SLAB_DEFINE(slot_pool, sizeof(Slot), CONFIG_HCP_COLLECTOR_SLOTS, 4);
K_MSGQ_DEFINE(inbox, sizeof(Slot*), CONFIG_HCP_COLLECTOR_QUEUE_DEPTH,
              sizeof(void*));

registry::Table g_table;
registry::StorageKeyValue g_kv;
modbus::Master g_bus;
Schedule g_sched;
BusHealth g_health;
HeartbeatWatchdog g_hb;

#ifdef CONFIG_HCP_ENABLE_SWITCH_CONTROL
/*
 * The relay outputs are a second device on the broker ("<sn>-K"), with their own commands,
 * state and report. They share this worker for one reason: switching a relay writes flash
 * (its persisted state), and the MQTT session thread must never block - the previous
 * firmware's SwitchController ran the same GPIO + NVS + publish sequence from its single
 * event-loop task. Nothing here touches the Modbus bus: the relays are board GPIOs
 * (hcp-k1/hcp-k2), not registers.
 */
switchctl::Relay g_relays[switchctl::kRelayCount];
bool g_relays_ready = false;
bool g_session_was_up = false;
constexpr int kMaxSwitchCommands = 4;

// Persisted per-relay state, under the same keys the previous firmware used for the
// same purpose (its NVS namespace "switch_state", fields k1_state/k2_state).
void RelayKey(int index, char* out, size_t cap) {
  snprintk(out, cap, "relay/k%d", index + 1);
}

bool LoadRelayState(int index) {
  char key[12];
  RelayKey(index, key, sizeof(key));
  char val[4];
  return g_kv.GetString(key, val, sizeof(val)) && val[0] == '1';
}

void SaveRelayState(int index, bool on) {
  char key[12];
  RelayKey(index, key, sizeof(key));
  if (!g_kv.SetString(key, on ? "1" : "0")) {
    COL_LOGW("relay K%d switched but not persisted; it returns to %s on reboot", index + 1,
             on ? "off" : "on");
  }
}

/* "<sn>-K/notify": the only place Home Assistant reads relay state from
 * (Switches.handle_notify). It is a full snapshot rather than a delta, because the
 * plugin's own view is a dict it updates key by key - a partial report leaves the
 * other switches showing whatever they last happened to hear. */
void PublishRelayStates() {
  bool states[switchctl::kRelayCount];
  for (int i = 0; i < switchctl::kRelayCount; i++) {
    states[i] = g_relays[i].Get();
  }
  char doc[80];
  const int n = switchctl::FormatNotify(states, switchctl::kRelayCount, doc,
                                        sizeof(doc));
  if (n == 0) {
    COL_LOGW("relay state document did not fit, nothing published");
    return;
  }
  if (!mqtt::PublishTopic("notify", doc, static_cast<size_t>(n),
                          mqtt::Tree::kSwitch)) {
    COL_LOGD("relay report deferred: no session yet");
  }
}

/* The availability document for the relay device, handed to hcp::mqtt so it goes out
 * on every connect. Home Assistant creates the switch entities from "switch_count"
 * and ignores the message without it (Switches.handle_online). */
int SwitchOnlineDoc(char* buf, size_t cap) {
  return switchctl::FormatOnline(switchctl::kRelayCount,
                                 CONFIG_HCP_MQTT_FW_VERSION, buf, cap);
}
#endif  // CONFIG_HCP_ENABLE_SWITCH_CONTROL

/* Scratch for one poll: the widest single read the table can ask for, and the
 * report built from it. Static rather than on the stack because this thread is the
 * only one that ever uses it, and the worker stack is 2 KB on the ESP32-C5. */
uint16_t g_regs[registry::kMaxRegisters];
uint8_t g_report[registry::kNotifyHeaderBytes + 2 * registry::kMaxRegisters];

bool g_bus_open = false;

const char* Name(Command cmd) {
  switch (cmd) {
    case Command::kConfig: return "config";
    case Command::kModbusWrite: return "modbus write";
    case Command::kRestart: return "restart";
    case Command::kReconfig: return "reconfig";
    case Command::kHeartbeat: return "heartbeat";
    case Command::kLed: return "led";
    case Command::kOtaStart: return "ota/start";
    case Command::kSwitchWrite: return "switch write";
    default: return "unknown";
  }
}

/* Runs on the MQTT session thread: copy, post, get out. Blocking here would stall
 * the whole session, including the periodic state publish. */
void OnMessage(const uint8_t* payload, size_t len, void* user) {
  const Command cmd = static_cast<Command>(reinterpret_cast<uintptr_t>(user));
  if (len > sizeof(Slot::data)) {
    COL_LOGW("%s payload of %u bytes does not fit, dropped", Name(cmd),
             (unsigned)len);
    return;
  }
  Slot* slot = nullptr;
  if (k_mem_slab_alloc(&slot_pool, reinterpret_cast<void**>(&slot),
                       K_NO_WAIT) != 0) {
    COL_LOGW("%s dropped: no message buffer left (%d slots)", Name(cmd),
             CONFIG_HCP_COLLECTOR_SLOTS);
    return;
  }
  slot->cmd = cmd;
  slot->len = static_cast<uint16_t>(len);
  memcpy(slot->data, payload, len);

  if (k_msgq_put(&inbox, &slot, K_NO_WAIT) != 0) {
    k_mem_slab_free(&slot_pool, slot);
    COL_LOGW("%s dropped: collector queue full (%d deep)", Name(cmd),
             CONFIG_HCP_COLLECTOR_QUEUE_DEPTH);
  }
}

void ApplyConfig(Slot& slot) {
  const char* text = reinterpret_cast<const char*>(slot.data);
  const registry::Apply result =
      g_table.ApplyConfig(text, slot.len, g_kv);
  switch (result) {
    case registry::Apply::kIgnored:
      /* No "segments" array - another plugin's handshake (Switches.py sends "{}"
       * on this very topic). Silent at info level, visible at debug. */
      COL_LOGD("config without segments, kept %d segment(s)", g_table.count());
      return;
    case registry::Apply::kRejected:
      COL_LOGW("config rejected (malformed or out of range); keeping %d segment(s)",
               g_table.count());
      return;
    case registry::Apply::kUnchanged:
      COL_LOGI("config unchanged (%d segment(s)), not rewritten",
               g_table.count());
      return;
    case registry::Apply::kUpdatedVolatile:
      COL_LOGW("config applied (%d segment(s)) but NOT saved: it is lost on reboot",
               g_table.count());
      break;
    case registry::Apply::kUpdated:
      COL_LOGI("config applied: %d segment(s) saved", g_table.count());
      break;
  }
  /* New points mean a new polling plan, and the old failure counts belong to the
   * devices that are no longer on the list: a unit that was silent yesterday may
   * be the meter somebody just wired up, so it must be probed again now rather
   * than left in a 30 s backoff inherited from before it existed. */
  g_sched.Rebuild(g_table, (uint64_t)k_uptime_get(),
                  CONFIG_HCP_COLLECTOR_POLL_MS);
  g_health.Clear();
}

/* A write request from <sn>/control/cmd, executed on this thread - which is the
 * only one allowed on the bus, so a write can never interleave with a poll. */
void ExecuteWrite(Slot& slot) {
  registry::WriteRequest req{};
  if (registry::ParseWriteRequest(slot.data, slot.len, &req) !=
      registry::WriteParse::kOk) {
    COL_LOGW("write request rejected (%u bytes, fc 0x%02x)", (unsigned)slot.len,
             slot.len > 1 ? slot.data[1] : 0);
    return;
  }
  if (!g_bus_open) {
    COL_LOGW("write to unit %u refused: bus not open", req.unit_id);
    return;
  }

  bool ok = false;
  if (req.function == 0x05) {
    ok = g_bus.WriteCoil(req.unit_id, req.start_address, req.coil_on);
  } else if (req.function == 0x06) {
    ok = g_bus.WriteSingleRegister(req.unit_id, req.start_address, req.values[0]);
  } else {
    ok = g_bus.WriteMultipleRegisters(req.unit_id, req.start_address, req.count,
                                      req.values);
  }
  COL_LOGI("write unit %u fc 0x%02x addr %u n %u -> %s", req.unit_id,
           req.function, req.start_address, req.count, ok ? "ok" : "failed");
  /* A slave that answers a write is alive, whatever its read history said. */
  g_health.NoteResult(req.unit_id, ok, (uint64_t)k_uptime_get());
}

#ifdef CONFIG_HCP_ENABLE_SWITCH_CONTROL
/* "<sn>-K/control/cmd", applied here on the worker. The payload is JSON naming one or
 * more switches, which is a different contract from the binary block the meter takes
 * on <sn>/control/cmd - the two are kept apart by the topic tree, not by guessing
 * from the payload shape. */
void ExecuteSwitch(Slot& slot) {
  switchctl::SwitchCommand cmds[kMaxSwitchCommands];
  const int n =
      switchctl::ParseCommands(slot.data, slot.len, cmds, kMaxSwitchCommands);
  if (n == 0) {
    /* Nothing usable, including for the truncated-object case: ParseCommands refuses a
     * payload that ends mid-way rather than acting on the part it could read, because
     * a relay controls a load and half a command is not a request anybody made. */
    COL_LOGW("switch command of %u bytes names no switch, ignored", (unsigned)slot.len);
    return;
  }
  if (!g_relays_ready) {
    COL_LOGW("switch command refused: relays unavailable");
    return;
  }

  for (int i = 0; i < n; i++) {
    const unsigned index = cmds[i].index;
    if (index < 1 || index > switchctl::kRelayCount) {
      COL_LOGW("switch_%u is not an output of this board [1, %d], ignored", index,
               switchctl::kRelayCount);
      continue;
    }
    switchctl::Relay& relay = g_relays[index - 1];
    if (relay.Get() != cmds[i].on) {
      relay.Set(cmds[i].on);
      SaveRelayState(static_cast<int>(index) - 1, cmds[i].on);
    }
    COL_LOGI("relay K%u -> %s", index, cmds[i].on ? "on" : "off");
  }
  if (n == kMaxSwitchCommands) {
    COL_LOGW("switch command carried at least %d keys; any further ones ignored", n);
  }
  /* Reported whether or not something changed: this is the point where the device
   * answers, and staying quiet when a command matched no output would leave Home
   * Assistant showing whatever the UI last guessed. */
  PublishRelayStates();
}
#endif  // CONFIG_HCP_ENABLE_SWITCH_CONTROL

/* One segment, one transaction, one report. The payload is the raw block, in the
 * byte order Home Assistant decodes - registry::PackRegisters owns that layout and
 * its host tests pin it, so nothing here is free to "improve" it. */
void Poll(int index) {
  const registry::Segment& seg = g_table.at(index);
  const uint64_t now = (uint64_t)k_uptime_get();

  if (!g_bus_open) {
    return;
  }
  if (seg.IsBitAccess()) {
    /* FC01/FC02 accepted by the table, deliberately not polled: the packed-bit
     * layout the response would need has no vector to check it against (none of
     * the device_protocol JSONs uses bit access), and guessing an order here is
     * exactly the silent-garbage failure the packing tests exist to prevent. */
    COL_LOGW("segment unit %u fc 0x%02x needs bit access, not implemented",
             seg.slave_id, seg.read_command);
    g_sched.Done(index, now, CONFIG_HCP_COLLECTOR_POLL_MS);
    return;
  }

  bool ok;
  if (seg.read_command == 0x04) {
    ok = g_bus.ReadInputRegisters(seg.slave_id, seg.start_address, seg.length,
                                 g_regs);
  } else {
    ok = g_bus.ReadHoldingRegisters(seg.slave_id, seg.start_address, seg.length,
                                   g_regs);
  }

  const uint64_t after = (uint64_t)k_uptime_get();
  g_health.NoteResult(seg.slave_id, ok, after);
  g_sched.Done(index, after, CONFIG_HCP_COLLECTOR_POLL_MS);

  if (!ok) {
    COL_LOGW("poll unit %u fc 0x%02x addr %u n %u failed (err=%d, %llu ms)",
             seg.slave_id, seg.read_command, seg.start_address, seg.length,
             g_bus.last_error(), (unsigned long long)(after - now));
    return;
  }

  const size_t n =
      registry::PackRegisters(seg, g_regs, seg.length, g_report, sizeof(g_report));
  if (n == 0) {
    COL_LOGW("unit %u: %u registers do not fit the report buffer", seg.slave_id,
             seg.length);
    return;
  }
  if (!mqtt::PublishTopic("notify", g_report, n)) {
    COL_LOGW("notify publish failed (%u bytes)", (unsigned)n);
  } else {
    COL_LOGD("notify unit %u addr %u n %u (%u bytes)", seg.slave_id,
             seg.start_address, seg.length, (unsigned)n);
  }
}

void Handle(Slot& slot) {
  switch (slot.cmd) {
    case Command::kConfig:
      ApplyConfig(slot);
      return;

    case Command::kModbusWrite:
      ExecuteWrite(slot);
      return;

    case Command::kHeartbeat: {
      const uint64_t now = (uint64_t)k_uptime_get();
      const bool was_armed = g_hb.armed();
      g_hb.Beat(now);
      if (!was_armed) {
        COL_LOGI("supervision heartbeat from Home Assistant: armed (60 s timeout)");
      }
      return;
    }

    case Command::kRestart:
      COL_LOGI("restart requested; rebooting in 250 ms so the log gets out");
      k_msleep(250);
      kernel::Reboot();
      return;

    case Command::kReconfig:
#ifdef CONFIG_HCP_PROVISION
      /* Same action as a long press on the button: forget the network and open
       * BLE again. Done through hcp::provision, which owns those keys. */
      COL_LOGI("reconfig requested: forgetting credentials, reopening BLE");
      provision::RequestReset();
#else
      COL_LOGW("reconfig requested but hcp::provision is not in this build; "
               "nothing erased");
#endif
      return;

    case Command::kLed: {
      /* Home Assistant's light entity. The strip belongs to hcp::provision (it is the
       * status readout), so "on"/"off" here means show the status light / keep it dark,
       * and the answer goes back in diagnostics as "led", which is where the
       * integration reads the light's state from. */
      const registry::LedSetting s =
          registry::ParseLedCommand(slot.data, slot.len);
      if (s == registry::LedSetting::kInvalid) {
        COL_LOGW("led command of %u bytes is neither \"on\" nor \"off\", ignored",
                 (unsigned)slot.len);
        return;
      }
#ifdef CONFIG_HCP_PROVISION
      provision::SetLedEnabled(s == registry::LedSetting::kOn);
#else
      COL_LOGW("led command %s but hcp::provision is not in this build",
               s == registry::LedSetting::kOn ? "on" : "off");
#endif
      return;
    }

    case Command::kOtaStart:
#ifdef CONFIG_HCP_OTA
      /* Home Assistant driving the update, as in the previous firmware (it registered
       * this topic and ran checkForUpdate/startUpdate against
       * http://<discovered host>:9998/firmware.bin). No URL arrives in the payload, so
       * the endpoints come from the configured templates plus the discovered host - and
       * if nothing serves that host, the failure is reported here rather than looking
       * like a device that ignores its owner. */
      COL_LOGI("ota/start: checking for an update from the discovered host");
      if (!ota::Trigger(nullptr)) {
        COL_LOGW("ota/start: not started (worker busy, no host discovered yet, or "
                 "the version endpoint is unreachable)");
      }
#else
      COL_LOGW("ota/start ignored: hcp::ota is not in this build");
#endif
      return;

    /* The case is compiled whether or not relay control is in the build, because
     * Classify() claims the suffix either way - so a device that receives a switch
     * command it cannot act on says so instead of going quiet. */
    case Command::kSwitchWrite:
#ifdef CONFIG_HCP_ENABLE_SWITCH_CONTROL
      ExecuteSwitch(slot);
#else
      COL_LOGW("switch command received but relay control is not compiled in "
               "(CONFIG_HCP_ENABLE_SWITCH_CONTROL=n)");
#endif
      return;

    default:
      return;
  }
}

void Worker(void* /*arg*/, void* /*p2*/, void* /*p3*/) {
  const registry::Source source = g_table.Load(g_kv);
  switch (source) {
    case registry::Source::kDownloaded:
      COL_LOGI("point table loaded from storage: %d segment(s)", g_table.count());
      break;
    case registry::Source::kBuiltin:
      COL_LOGI("point table: %d built-in segment(s)", g_table.count());
      break;
    default:
      COL_LOGI("no point table yet; waiting for config from Home Assistant");
      break;
  }

  /* The bus is opened here rather than in Start() so the (possibly failing) device
   * lookup happens on this thread and the point table is already loaded when the
   * first poll is scheduled. */
  modbus::Config cfg;
  cfg.baud = CONFIG_HCP_MODBUS_DEFAULT_BAUD;
  cfg.response_timeout_ms = CONFIG_HCP_MODBUS_TIMEOUT_MS;
  g_bus_open = g_bus.Open(cfg);
  if (!g_bus_open) {
    COL_LOGW("modbus bus unavailable; polling disabled (config still accepted)");
  }
  g_sched.Rebuild(g_table, (uint64_t)k_uptime_get(),
                  CONFIG_HCP_COLLECTOR_POLL_MS);

#ifdef CONFIG_HCP_ENABLE_SWITCH_CONTROL
  /* Relays come up in the state they were left in, before anything is reported - the
   * previous firmware's SwitchController::init() did the same (NVS load, then
   * setGpioLevel), and the order is what makes the first report truthful about the
   * pins rather than about what Home Assistant last asked for. */
  int ready = 0;
  for (int i = 0; i < switchctl::kRelayCount; i++) {
    if (!g_relays[i].Open(i)) {
      COL_LOGW("relay K%d unavailable", i + 1);
      continue;
    }
    const bool on = LoadRelayState(i);
    g_relays[i].Set(on);
    COL_LOGI("relay K%d restored to %s", i + 1, on ? "on" : "off");
    ready++;
  }
  g_relays_ready = ready == switchctl::kRelayCount;
#endif

  while (true) {
    Slot* slot = nullptr;
    if (k_msgq_get(&inbox, &slot,
                   K_MSEC(CONFIG_HCP_COLLECTOR_TICK_MS)) == 0) {
      if (slot != nullptr) {
        Handle(*slot);
        k_mem_slab_free(&slot_pool, slot);
      }
    }

    const uint64_t now = (uint64_t)k_uptime_get();
    if (g_hb.Expired(now)) {
      /* One-shot by construction: rebooting never returns, so there is no
       * "expired again next tick" state to guard. */
      COL_LOGW("no supervision heartbeat for %llu ms - Home Assistant is gone, "
               "rebooting",
               (unsigned long long)g_hb.SinceBeat(now));
      k_msleep(250);
      kernel::Reboot();
    }

    if (g_bus_open && g_table.pollable()) {
      const int index = g_sched.NextDue(g_table, g_health, now);
      if (index >= 0) {
        Poll(index);
      }
    }

#ifdef CONFIG_HCP_ENABLE_SWITCH_CONTROL
    /* Re-report the relay states whenever the session comes up: Home Assistant
     * subscribes QoS 0 and keeps no queue for a device that was away, so without this its
     * switches show whatever they remembered until the next command. An edge, not a tick -
     * the topic carries state, not a heartbeat. */
    const bool link_up = mqtt::IsConnected();
    if (link_up && !g_session_was_up && g_relays_ready) {
      PublishRelayStates();
    }
    g_session_was_up = link_up;
#endif
  }
}

K_THREAD_STACK_DEFINE(worker_stack, CONFIG_HCP_COLLECTOR_STACK_SIZE);
struct k_thread worker_thread;
bool started;

}  // namespace

const registry::Table& Table() { return g_table; }

bool Start() {
  if (started) {
    return true;
  }

  /* One route per command. The Command is passed through the user pointer so the
   * shared callback does not need a closure per topic. */
  struct Bind {
    const char* suffix;
    Command cmd;
    mqtt::Tree tree;
  };
  const Bind binds[] = {
      {"config", Command::kConfig, mqtt::Tree::kMain},
      {"control/cmd", Command::kModbusWrite, mqtt::Tree::kMain},
      {"control/restart", Command::kRestart, mqtt::Tree::kMain},
      {"control/reconfig", Command::kReconfig, mqtt::Tree::kMain},
      {"control/led", Command::kLed, mqtt::Tree::kMain},
      {"host/heartbeat", Command::kHeartbeat, mqtt::Tree::kMain},
      {"ota/start", Command::kOtaStart, mqtt::Tree::kMain},
#ifdef CONFIG_HCP_ENABLE_SWITCH_CONTROL
      /* The relay device's channel, which shares the suffix name with the meter's
       * binary write channel and is told apart by its tree. */
      {"control/cmd", Command::kSwitchWrite, mqtt::Tree::kSwitch},
#endif
  };
  for (const Bind& b : binds) {
    if (!mqtt::Subscribe(b.suffix, OnMessage,
                         reinterpret_cast<void*>(static_cast<uintptr_t>(b.cmd)),
                         b.tree)) {
      COL_LOGW("cannot subscribe to <%s>", b.suffix);
      return false;
    }
  }

#ifdef CONFIG_HCP_ENABLE_SWITCH_CONTROL
  /* Announce the relay device on every connect - hcp::mqtt repeats "<sn>-K/online",
   * because the session can come up long after this call and a document published once at
   * boot would miss every session after it. The direct publish below covers the other
   * order (a session that came up between mqtt::Start() and here) and is harmless when it
   * fails, since retained messages are replaced. */
  if (!mqtt::AnnounceDevice(mqtt::Tree::kSwitch, SwitchOnlineDoc)) {
    COL_LOGW("cannot announce the relay device on <sn>-K/online");
    return false;
  }
  char online[80];
  const int online_len = SwitchOnlineDoc(online, sizeof(online));
  if (online_len > 0 &&
      !mqtt::PublishRetainedTopic("online", online, static_cast<size_t>(online_len),
                                  mqtt::Tree::kSwitch)) {
    COL_LOGD("relay device not announced yet: no session (will be sent on connect)");
  }
#endif

  k_thread_create(&worker_thread, worker_stack, K_THREAD_STACK_SIZEOF(worker_stack),
                  Worker, nullptr, nullptr, nullptr,
                  /* Preemptive, not cooperative: this thread will block on the
                   * Modbus bus (a read waits for the slave), and a cooperative
                   * priority would make that wait stall the whole system. */
                  5, 0, K_NO_WAIT);
  k_thread_name_set(&worker_thread, "hcp_collect");
  started = true;
  return true;
}

}  // namespace collector
}  // namespace hcp

#endif /* CONFIG_HCP_COLLECTOR */
