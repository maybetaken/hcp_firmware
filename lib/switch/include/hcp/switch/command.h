/*
 * hcp::switchctl - the JSON shapes the relay device exchanges with Home Assistant.
 *
 * The relay outputs are a device of their own on the broker, addressed "<sn>-K" (a
 * sibling of "<sn>" - see hcp/mqtt/client.h), speaking JSON rather than the raw Modbus
 * blocks the meter device uses. These shapes are what the two sides actually do:
 *
 *   in   <sn>-K/control/cmd  {"switch_01": "on"}          - the previous firmware's
 *                             handler (main/solar_manager.cpp:308) and what HA's
 *                             JsonProtocolHelper packs for a write
 *   out  <sn>-K/notify       {"switch_01": "on", ...}     - the only state topic
 *                             Switches.handle_notify subscribes
 *   out  <sn>-K/online       {"switch_count": 2, ...}     - Switches.handle_online,
 *                             which rejects the payload unless switch_count is an int
 *                             >= MIN_SWITCH_COUNT (2)
 *
 * Names are one-based and zero-padded to two digits (plugins/Switches.py switch_name():
 * `f"switch_{index:02d}"` is the authority); states are the strings "on"/"off", because
 * handle_notify skips anything else with a warning.
 *
 * No JSON library: the payloads are flat objects of string values, the registry already
 * shows how a hand-rolled scanner stays host-tested, and a bundled parser would bring a
 * dependency and a malloc pattern this port exists to remove.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace hcp {
namespace switchctl {

// One switch named in a control/cmd object.
struct SwitchCommand {
  uint8_t index;  // one-based: 1 == "switch_01" == the first relay
  bool on;
};

/*
 * Read every "switch_NN" key of a control/cmd object.
 *
 * Writes at most `max` commands into `out` and returns how many it wrote, so the
 * caller can tell "no keys matched" (0) from "the message is bigger than I can act
 * on" (== max). A payload that is not an object, or whose value is neither "on" nor
 * "off", yields no command for that key rather than a partial guess: Home Assistant
 * logs a warning for a state it cannot parse, and silently doing something else is
 * worse on a relay that switches a load.
 *
 * `buf` is the MQTT payload: length-bounded and NOT NUL terminated.
 */
int ParseCommands(const uint8_t* buf, size_t len, SwitchCommand* out, int max);

/*
 * Format the <sn>-K/notify state report: one "switch_NN":"on"|"off" pair per entry
 * of `states`, in index order.
 *
 * Returns the length written, or 0 when the buffer cannot hold it. Emitting a
 * truncated document is refused on purpose - a half-written JSON object makes the
 * integration's parse fail outright and the whole device look dead in the UI, while
 * saying nothing at all just leaves the previous state shown.
 */
int FormatNotify(const bool* states, int count, char* buf, size_t cap);

/*
 * Format the <sn>-K/online availability document.
 *
 * `switch_count` is written as a bare integer because the integration type-checks it
 * (it warns and ignores the payload on "Online 'switch_count' ... is not an int"),
 * and it has to be >= 2 for Home Assistant to accept this device at all.
 * Returns the length written, or 0 if it does not fit.
 */
int FormatOnline(int switch_count, const char* version, char* buf, size_t cap);

}  // namespace switchctl
}  // namespace hcp
