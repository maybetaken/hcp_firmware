/*
 * hcp::collector — the business loop: what Home Assistant asks for, what gets
 * polled, and on which thread.
 *
 * The old firmware ran two paths against the same hardware: MQTT callbacks posted
 * events to a queue consumed by the main task, while a separate polling task read
 * the bus and published to MQTT directly - inside the mutex that was supposed to
 * serialise the two. That is where the tangle came from, and the shape that
 * replaces it is one queue and one worker:
 *
 *   mqtt session thread ──copy into a slab slot──▶ k_msgq ──▶ worker thread
 *                                                        (registry, modbus bus,
 *                                                         MQTT publishing)
 *
 * Rules that matter and are therefore stated:
 *
 * - Only the worker touches the Modbus bus and the point table, so there is no
 *   bus mutex to forget and no publishing from inside one.
 * - The MQTT route callbacks (hcp::mqtt::Subscribe) run on the session thread and
 *   must not block: they copy and post. A message that does not fit the queue is
 *   dropped with a warning - Home Assistant re-sends config on every start, and a
 *   stalled collector is worse than a discarded publish.
 * - Payloads are fixed size from a mem_slab, never malloc: the version this
 *   replaces handed raw pointers through the queue and freed them in each case of
 *   a switch, which is how a leak survives review.
 *
 * Google C++ Style naming.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "hcp/registry/table.h"

namespace hcp {
namespace collector {

/* What an inbound topic under "<sn>/" means. Kept as a pure function of the
 * suffix so the mapping - which is a contract with Home Assistant, not an
 * implementation detail - has host tests instead of only a switch buried in a
 * thread loop. */
enum class Command {
  kNone = 0,
  kConfig,       /* <sn>/config: the point table, as JSON */
  kModbusWrite,  /* <sn>/control/cmd: raw [unit, fc, addr..., values...] */
  kRestart,      /* <sn>/control/restart */
  kReconfig,     /* <sn>/control/reconfig: forget Wi-Fi, open BluFi */
  kHeartbeat,    /* <sn>/host/heartbeat: the host is watching */
  kLed,          /* <sn>/control/led: "on"/"off" */
};

/* suffix is length-bounded and NOT NUL terminated (it comes off the MQTT packet). */
Command Classify(const char* suffix, size_t len);

/* Registers the MQTT routes, loads the persisted point table and starts the
 * worker. Idempotent; false if the thread could not start. */
bool Start();

/* The table the worker is using. Read-only outside the worker. */
const registry::Table& Table();

}  // namespace collector
}  // namespace hcp
