/*
 * hcp::collector - the business loop: what Home Assistant asks for, what gets polled,
 * and on which thread. One queue and one worker:
 *
 *   mqtt session thread --copy into a slab slot--> k_msgq --> worker thread
 *                                        (registry, modbus bus, MQTT publishing)
 *
 * The firmware this replaced ran two paths against the same hardware - MQTT callbacks
 * posting to a queue consumed by the main task, and a polling task reading the bus and
 * publishing inside the mutex that was meant to serialise them.
 *
 * - Only the worker touches the bus and the point table, so there is no bus mutex to
 *   forget and no publishing from inside one.
 * - The MQTT route callbacks run on the session thread and must not block: they copy and
 *   post. A message that does not fit is dropped with a warning - HA re-sends config on
 *   every start, and a stalled collector is worse than a discarded publish.
 * - Payloads are fixed size from a mem_slab, never malloc: the old code handed raw
 *   pointers through the queue and freed them in each case of a switch.
 *
 * Google C++ Style naming.
 *
 * Copyright (c) 2026 maybetaken
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "hcp/mqtt/client.h"
#include "hcp/registry/table.h"

namespace hcp {
namespace collector {

/* What an inbound topic under a device prefix means. Kept as a pure function of the
 * tree and the suffix so the mapping - which is a contract with Home Assistant, not
 * an implementation detail - has host tests instead of only a switch buried in a
 * thread loop.
 *
 * The tree is part of the input because the same suffix means different things in
 * the two identities a Helios board publishes: "control/cmd" under "<sn>" carries a
 * raw Modbus write for the meter, and under "<sn>-K" it carries a JSON switch
 * command for the relays. Answering either with the other's handler would not throw
 * an exception, it would switch the wrong load. */
enum class Command {
  kNone = 0,
  kConfig,       /* <sn>/config: the point table, as JSON (per-unit merge) */
  kReplaceConfig, /* same JSON, but replaces the WHOLE table: the firmware's own device
                   * table on a device-type change, where the previous type's segments
                   * must not survive. Internal only - no MQTT suffix maps to it. */
  kModbusWrite,  /* <sn>/control/cmd: raw [unit, fc, addr..., values...] */
  kRestart,      /* <sn>/control/restart */
  kReconfig,     /* <sn>/control/reconfig: forget Wi-Fi, open BluFi */
  kHeartbeat,    /* <sn>/host/heartbeat: the host is watching */
  kLed,          /* <sn>/control/led: "on"/"off" */
  kOtaStart,     /* <sn>/ota/start: check for an update and take it (the previous
                  * firmware's own trigger: solar_manager.cpp registered this topic
                  * and did checkForUpdate() then startUpdate(), against
                  * http://<host from SSDP>:9998/firmware.bin + version.txt) */
  kSwitchWrite,  /* <sn>-K/control/cmd: {"switch_01":"on"} for the relay outputs */
};

/* suffix is length-bounded and NOT NUL terminated (it comes off the MQTT packet). */
Command Classify(const char* suffix, size_t len,
                 mqtt::Tree tree = mqtt::Tree::kMain);

/* Registers the MQTT routes, loads the persisted point table and starts the
 * worker. Idempotent; false if the thread could not start. */
bool Start();

/* The table the worker is using. Read-only outside the worker. */
const registry::Table& Table();

}  // namespace collector
}  // namespace hcp
