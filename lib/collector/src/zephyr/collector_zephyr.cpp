/*
 * hcp::collector — Zephyr side: the queue, the worker thread and the MQTT routes.
 * See collector.h for why there is exactly one thread allowed to touch the bus.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "hcp/collector/collector.h"

#ifdef CONFIG_HCP_COLLECTOR

#include <string.h>

#include <zephyr/kernel.h>

#include "hcp/log/log.h"
#include "hcp/mqtt/client.h"
#include "hcp/registry/storage_kv.h"

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

const char* Name(Command cmd) {
  switch (cmd) {
    case Command::kConfig: return "config";
    case Command::kModbusWrite: return "modbus write";
    case Command::kRestart: return "restart";
    case Command::kReconfig: return "reconfig";
    case Command::kHeartbeat: return "heartbeat";
    case Command::kLed: return "led";
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
      return;
    case registry::Apply::kUpdated:
      COL_LOGI("config applied: %d segment(s) saved", g_table.count());
      return;
  }
}

void Handle(Slot& slot) {
  switch (slot.cmd) {
    case Command::kConfig:
      ApplyConfig(slot);
      return;
    case Command::kModbusWrite:
    case Command::kRestart:
    case Command::kReconfig:
    case Command::kHeartbeat:
    case Command::kLed:
      /* Received on the right topic, routed, on the right thread - deliberately
       * not acted on yet: each of those needs the subsystem it belongs to wired to
       * this queue (polling/reporting is the next step). Said out loud so nobody
       * reads a silent topic as a working feature. */
      COL_LOGI("%s received (%u bytes), not wired up yet", Name(slot.cmd),
               (unsigned)slot.len);
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

  while (true) {
    Slot* slot = nullptr;
    if (k_msgq_get(&inbox, &slot,
                   K_MSEC(CONFIG_HCP_COLLECTOR_TICK_MS)) == 0) {
      if (slot != nullptr) {
        Handle(*slot);
        k_mem_slab_free(&slot_pool, slot);
      }
    }
    /* The tick is the seam the poller will hang off next: waiting on the queue
     * with a timeout is what lets one thread serve both. */
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
  };
  const Bind binds[] = {
      {"config", Command::kConfig},
      {"control/cmd", Command::kModbusWrite},
      {"control/restart", Command::kRestart},
      {"control/reconfig", Command::kReconfig},
      {"control/led", Command::kLed},
      {"host/heartbeat", Command::kHeartbeat},
  };
  for (const Bind& b : binds) {
    if (!mqtt::Subscribe(b.suffix, OnMessage,
                         reinterpret_cast<void*>(static_cast<uintptr_t>(b.cmd)))) {
      COL_LOGW("cannot subscribe to <%s>", b.suffix);
      return false;
    }
  }

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
