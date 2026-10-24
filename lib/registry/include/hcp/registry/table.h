/*
 * hcp::registry — the Modbus point table the collector polls, and the rules for
 * where a table came from.
 *
 * The wire contract with Home Assistant (unchanged from the previous firmware,
 * verified against the plugin modules under
 * custom_components/solar_manager/plugins):
 *
 *     <sn>/config  ->  {"segments":[{"slave_id":1,"start_address":8192,
 *                                    "length":16,"read_command":3}, ...]}
 *
 * Nothing else on that topic is interpreted: registers, names, scaling and units
 * stay on the HA side, so the device passes raw blocks through and the point table
 * has exactly one copy in the world (the HA device_protocol JSON).
 *
 * Precedence, persistence and the quiet cases are the whole point of this file, so
 * they are stated here and implemented once:
 *
 *   - A downloaded table always wins over the built-in default, and wins again on
 *     every re-download (the old firmware latched the first one and ignored the
 *     rest, so changing a table needed a reboot).
 *   - A download replaces **the unit addresses it mentions**, and leaves other
 *     slaves' segments alone. That is what lets one RS-485 bus carry several
 *     sub-devices: Home Assistant publishes one plugin's segments at a time, and
 *     the last one published must not erase the meters before it.
 *   - A download that differs from what is running is written to storage, and what
 *     is running is what gets polled. A download identical to the current table is
 *     dropped WITHOUT writing - NVS is flash, and HA re-sends config on every
 *     startup, so "no change" is the common case.
 *   - Payloads that carry no "segments" array at all are ignored, not treated as
 *     empty. Switches.py publishes "{}" on that topic for handshake parity, and
 *     honouring it as a table would erase a working meter configuration.
 *   - {"segments":[]} IS an explicit empty table and is honoured - polling stops.
 *     It persists as "no table" (there is nothing to record), which is the same
 *     state a reboot should come back to, so the stored key is simply not written.
 *   - When the downloaded table equals the one in use, nothing is written; the
 *     provenance is still upgraded to kDownloaded, because HA re-sends config on
 *     every start and a confirmed default should not keep looking like a guess.
 *
 * The parsing here is deliberately narrow: it walks exactly the shape above and
 * rejects anything else, rather than pretending to be a JSON library. Unknown
 * members are skipped (HA adds them), nesting is tracked so a "length" inside a
 * nested object cannot be mistaken for a segment's, and numbers must be integers.
 * Strings, escapes, floats and exponents are NOT parsed - a payload containing
 * them where a number is expected is rejected, which is the safe direction: an
 * ignored config keeps the previous table instead of installing a wrong one.
 *
 * Pure logic: no Zephyr headers, no kernel, no storage - the caller supplies a
 * KeyValue interface, so the precedence/dedup rules are host-testable with a fake.
 * Google C++ Style naming.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace hcp {
namespace registry {

/* One contiguous register read. Mirrors the four fields HA sends. */
struct Segment {
  uint8_t slave_id;       /* Modbus unit address, 1..247 (0 is broadcast) */
  uint16_t start_address; /* first register/coil of the block */
  uint16_t length;        /* count, in registers (or bits for fc 1/2) */
  uint8_t read_command;   /* 0x01/0x02/0x03/0x04 */

  bool IsBitAccess() const { return read_command == 0x01 || read_command == 0x02; }
};

constexpr int kMaxSegments = 16; /* the widest single table in use is jkbms.json at
                                   * 5 segments; 16 leaves room for three sub-devices
                                   * on one bus. A download that would exceed it is
                                   * refused rather than silently truncated. */

/* Per-segment count limits, so an impossible table is rejected when it arrives
 * instead of failing every poll. The collector reads through Zephyr's subsys/modbus,
 * which builds the ADU in CONFIG_MODBUS_BUFFER_SIZE (max 256) bytes: FC03/FC04 fit
 * 2 bytes per register plus ~9 bytes of header, FC01/FC02 fit 8 bits per byte.
 * The real tables in the field (jkbms.json) ask for 100 registers, hence 123. */
constexpr uint16_t kMaxRegisters = 123;
constexpr uint16_t kMaxBits = 1800;

/* Where the running table came from. Polling never starts on a built-in default
 * that nobody confirmed, which is why the distinction is kept in the object. */
enum class Source {
  kEmpty = 0,     /* nothing loaded: the bus is left alone */
  kBuiltin,       /* compiled-in defaults */
  kDownloaded,    /* what HA gave us (and what storage holds) */
};

/* Result of applying a config payload - the caller logs it, and only the kUpdated
 * cases may touch flash. */
enum class Apply {
  kIgnored = 0,     /* payload carries no segments array (e.g. "{}" from
                       Switches.py): the current table stands */
  kUnchanged,       /* same table as now: nothing written to storage */
  kUpdated,         /* new table installed and persisted */
  kUpdatedVolatile, /* new table installed, but storage refused the write - it is
                       live until the next reboot, so it must not look durable */
  kRejected,        /* malformed or unusable payload: current table stands */
};

/* Storage seam. hcp::storage::Store satisfies this on target; tests use a fake. */
struct KeyValue {
  virtual ~KeyValue() = default;
  virtual bool GetString(const char* key, char* out, size_t cap) = 0;
  virtual bool SetString(const char* key, const char* val) = 0;
  virtual bool Erase(const char* key) = 0;
};

/* storage key for the serialised table, under the hcp::storage root */
extern const char kTableKey[];

class Table {
 public:
  /* Built-in defaults, used only when storage holds nothing. Empty by default:
   * which meter is on the bus is a product/installation fact, and guessing it
   * would poll the wrong slave. Board/product builds put their own entries here. */
  static const Segment* Builtin();
  static int BuiltinCount();

  int count() const { return count_; }
  const Segment& at(int i) const { return segs_[i]; }
  const Segment* data() const { return segs_; }
  Source source() const { return source_; }
  bool empty() const { return count_ == 0; }
  bool pollable() const { return count_ > 0; }

  bool operator==(const Table& o) const;
  bool operator!=(const Table& o) const { return !(*this == o); }

  /* Canonical text form, the only thing written to storage:
   *   "slave:start:length:fc;slave:start:length:fc"   (no trailing ';')
   * Stable, diffable, and small (a full table is well under 200 bytes). */
  size_t Serialize(char* out, size_t cap) const;
  bool Deserialize(const char* text); /* false if malformed or over capacity */

  /* Read the persisted table if there is one, else take the built-in defaults.
   * Returns the source actually used. A stored blob that no longer parses is
   * erased and defaults are used, so one bad write cannot wedge collection. */
  Source Load(KeyValue& kv);

  /* Apply a payload from the config topic, with the precedence and dedup rules
   * described at the top of this header. */
  Apply ApplyConfig(const char* payload, size_t len, KeyValue& kv);

 private:
  /* Value-initialised so padding is deterministic even though equality is
   * defined field by field (see table.cpp). */
  Segment segs_[kMaxSegments]{};
  int count_ = 0;
  Source source_ = Source::kEmpty;
};

/*
 * Parse only the shape we accept. Public for testing; normally reached through
 * ApplyConfig().
 *
 * Returns  kParsed on success (out_count may be 0 for an explicit empty array),
 *          kNoSegments when the payload has no "segments" array (a "{}" like the
 *              one Switches.py sends),
 *          kMalformed for anything else, including numbers that are not integers
 *              and tables larger than kMaxSegments.
 */
enum class Parsed {
  kMalformed = 0,
  kNoSegments,
  kParsed,
};
Parsed ParseWire(const char* payload, size_t len, Segment* out, int max,
                 int* out_count);

}  // namespace registry
}  // namespace hcp
