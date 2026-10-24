/*
 * hcp::registry - see table.h for the contract (precedence, dedup, the quiet
 * cases) and for what the parser deliberately does not accept.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "hcp/registry/table.h"

#include <stdio.h>
#include <string.h>

namespace hcp {
namespace registry {

const char kTableKey[] = "reg/table";

/* Which meter is on the bus is an installation fact, not a firmware fact: the
 * built-in table is empty until somebody puts one here for a specific product, so
 * an unprovisioned device simply does not poll. */
static const Segment kBuiltinNone[] = {};

const Segment* Table::Builtin() { return kBuiltinNone; }
int Table::BuiltinCount() { return (int)(sizeof(kBuiltinNone) / sizeof(kBuiltinNone[0])); }

namespace {

struct Cursor {
  const char* p;
  const char* end;
};

void SkipSpace(Cursor* c) {
  while (c->p < c->end && (*c->p == ' ' || *c->p == '\t' || *c->p == '\n' ||
                           *c->p == '\r')) {
    c->p++;
  }
}

bool Eat(Cursor* c, char ch) {
  SkipSpace(c);
  if (c->p >= c->end || *c->p != ch) {
    return false;
  }
  c->p++;
  return true;
}

/* A JSON string, without unescaping: we only need to know where it ends. Returns
 * the text between the quotes for keys (which are plain names in this payload). */
bool ReadString(Cursor* c, const char** out_begin, size_t* out_len) {
  SkipSpace(c);
  if (c->p >= c->end || *c->p != '"') {
    return false;
  }
  const char* begin = ++c->p;
  bool escaped = false;
  for (; c->p < c->end; c->p++) {
    if (escaped) {
      escaped = false;
      continue;
    }
    if (*c->p == '\\') {
      escaped = true;
      continue;
    }
    if (*c->p == '"') {
      if (out_begin != nullptr) {
        *out_begin = begin;
      }
      if (out_len != nullptr) {
        *out_len = (size_t)(c->p - begin);
      }
      c->p++; /* the closing quote */
      return true;
    }
  }
  return false;
}

bool KeyIs(const char* key, size_t len, const char* want) {
  return len == strlen(want) && memcmp(key, want, len) == 0;
}

/* Look at the next non-space character without consuming it. */
bool PeekIs(Cursor* c, char ch) {
  SkipSpace(c);
  return c->p < c->end && *c->p == ch;
}

/* Skip any value, tracking nesting, so unknown members (HA adds them) cannot be
 * mistaken for our own. */
bool SkipValue(Cursor* c) {
  SkipSpace(c);
  if (c->p >= c->end) {
    return false;
  }
  if (*c->p == '{' || *c->p == '[') {
    char open = *c->p;
    char close = (open == '{') ? '}' : ']';
    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (; c->p < c->end; c->p++) {
      if (in_string) {
        if (escaped) {
          escaped = false;
        } else if (*c->p == '\\') {
          escaped = true;
        } else if (*c->p == '"') {
          in_string = false;
        }
        continue;
      }
      if (*c->p == '"') {
        in_string = true;
      } else if (*c->p == open) {
        depth++;
      } else if (*c->p == close) {
        if (--depth == 0) {
          c->p++;
          return true;
        }
      }
    }
    return false; /* unbalanced */
  }
  if (*c->p == '"') {
    return ReadString(c, nullptr, nullptr);
  }
  /* number | true | false | null: run to the next delimiter */
  const char* start = c->p;
  while (c->p < c->end && *c->p != ',' && *c->p != '}' && *c->p != ']' &&
         *c->p != ' ' && *c->p != '\t' && *c->p != '\n' && *c->p != '\r') {
    c->p++;
  }
  return c->p != start;
}

/* An integer, and only an integer: a float or exponent here means the payload is
 * not the one we agreed on, and installing a guessed value would be worse than
 * ignoring the config. */
bool ReadUint(Cursor* c, uint32_t* out, uint32_t max) {
  SkipSpace(c);
  if (c->p >= c->end || *c->p < '0' || *c->p > '9') {
    return false;
  }
  uint32_t v = 0;
  const char* first = c->p;
  while (c->p < c->end && *c->p >= '0' && *c->p <= '9') {
    if (v > (max / 10U)) {
      return false; /* would overflow, or simply out of the agreed range */
    }
    v = v * 10U + (uint32_t)(*c->p - '0');
    c->p++;
  }
  if (c->p < c->end && (*c->p == '.' || *c->p == 'e' || *c->p == 'E')) {
    return false;
  }
  if (v > max) {
    return false;
  }
  *out = v;
  (void)first;
  return true;
}

bool ReadSegmentObject(Cursor* c, Segment* out) {
  if (!Eat(c, '{')) {
    return false;
  }
  uint32_t slave = 0, start = 0, length = 0, fc = 0;
  bool have[4] = {false, false, false, false};
  if (PeekIs(c, '}')) {
    c->p++;
    return false; /* an empty object carries no segment */
  }
  while (true) {
    const char* key;
    size_t klen;
    if (!ReadString(c, &key, &klen) || !Eat(c, ':')) {
      return false;
    }
    if (KeyIs(key, klen, "slave_id")) {
      have[0] = ReadUint(c, &slave, 247U);
      /* unit 0 is the broadcast address: it can never answer a read, so a segment
       * carrying it would cost a timeout on every poll cycle. Reject it here, at
       * the moment the table arrives, where the reason can still be reported. */
      if (have[0] && slave == 0U) {
        return false;
      }
      if (!have[0]) {
        return false;
      }
    } else if (KeyIs(key, klen, "start_address")) {
      have[1] = ReadUint(c, &start, 0xFFFFU);
      if (!have[1]) return false;
    } else if (KeyIs(key, klen, "length")) {
      have[2] = ReadUint(c, &length, 0xFFFFU);
      if (!have[2]) return false;
    } else if (KeyIs(key, klen, "read_command")) {
      have[3] = ReadUint(c, &fc, 0xFFU);
      if (!have[3]) return false;
    } else if (!SkipValue(c)) {
      return false;
    }
    if (Eat(c, ',')) {
      continue;
    }
    if (!Eat(c, '}')) {
      return false;
    }
    if (!(have[0] && have[1] && have[2] && have[3]) || length == 0U) {
      return false;
    }
    /* The count limit depends on whether the block is registers or bits. */
    const bool fits = (fc == 0x01U || fc == 0x02U) ? (length <= kMaxBits)
                                                  : (length <= kMaxRegisters);
    if (!fits) {
      return false;
    }
    out->slave_id = (uint8_t)slave;
    out->start_address = (uint16_t)start;
    out->length = (uint16_t)length;
    out->read_command = (uint8_t)fc;
    return true;
  }
}

}  // namespace

Parsed ParseWire(const char* payload, size_t len, Segment* out, int max,
                 int* out_count) {
  if (payload == nullptr || out_count == nullptr) {
    return Parsed::kMalformed;
  }
  *out_count = 0;

  Cursor c{payload, payload + len};
  if (!Eat(&c, '{')) {
    return Parsed::kMalformed;
  }
  bool saw_segments = false;
  if (PeekIs(&c, '}')) {
    c.p++;
    return Parsed::kNoSegments; /* "{}" - another plugin's handshake */
  }
  while (true) {
    const char* key;
    size_t klen;
    if (!ReadString(&c, &key, &klen) || !Eat(&c, ':')) {
      return Parsed::kMalformed;
    }
    if (!KeyIs(key, klen, "segments")) {
      if (!SkipValue(&c)) {
        return Parsed::kMalformed;
      }
    } else {
      if (!saw_segments) {
        saw_segments = true;
        *out_count = 0;
      }
      if (!Eat(&c, '[')) {
        return Parsed::kMalformed;
      }
      if (PeekIs(&c, ']')) {
        c.p++; /* {"segments":[]} - an explicit, honourable empty table */
      } else {
        while (true) {
          if (*out_count >= max) {
            return Parsed::kMalformed; /* refuse to truncate */
          }
          Segment seg;
          if (!ReadSegmentObject(&c, &seg)) {
            return Parsed::kMalformed;
          }
          out[(*out_count)++] = seg;
          if (Eat(&c, ',')) {
            continue;
          }
          if (!Eat(&c, ']')) {
            return Parsed::kMalformed;
          }
          break;
        }
      }
    }
    if (Eat(&c, ',')) {
      continue;
    }
    break;
  }
  if (!Eat(&c, '}')) {
    return Parsed::kMalformed;
  }
  return saw_segments ? Parsed::kParsed : Parsed::kNoSegments;
}

bool Table::operator==(const Table& o) const {
  if (count_ != o.count_) {
    return false;
  }
  /* Field by field, not memcmp: Segment carries alignment padding (a uint8_t
   * before two uint16_t), and comparing those bytes compares uninitialised
   * stack - which is exactly how a table that round-trips through storage can
   * still compare unequal to the one that was written. */
  for (int i = 0; i < count_; i++) {
    if (segs_[i].slave_id != o.segs_[i].slave_id ||
        segs_[i].start_address != o.segs_[i].start_address ||
        segs_[i].length != o.segs_[i].length ||
        segs_[i].read_command != o.segs_[i].read_command) {
      return false;
    }
  }
  return true;
}

size_t Table::Serialize(char* out, size_t cap) const {
  size_t used = 0;
  for (int i = 0; i < count_; i++) {
    /* "255:65535:65535:255" is 18 chars plus a separator; 24 keeps snprintf
     * bounds obvious. */
    char piece[24];
    int n = snprintf(piece, sizeof(piece), "%s%u:%u:%u:%u", (i == 0) ? "" : ";",
                     (unsigned)segs_[i].slave_id, (unsigned)segs_[i].start_address,
                     (unsigned)segs_[i].length, (unsigned)segs_[i].read_command);
    if (n < 0 || (size_t)n >= sizeof(piece) || used + (size_t)n >= cap) {
      out[used] = '\0';
      return 0;
    }
    memcpy(out + used, piece, (size_t)n);
    used += (size_t)n;
  }
  out[used] = '\0';
  return used;
}

bool Table::Deserialize(const char* text) {
  if (text == nullptr) {
    return false;
  }
  Segment tmp[kMaxSegments];
  int n = 0;
  const char* p = text;
  if (*p == '\0') {
    /* the empty table is a legal state ({"segments":[]}), but an empty string in
     * storage is not a table at all - callers treat it as "nothing stored". */
    return false;
  }
  /* Hand-scanned rather than sscanf(): picolibc's scanf support for %n is not
   * something this file should depend on. */
  while (true) {
    uint32_t field[4];
    int i;
    for (i = 0; i < 4; i++) {
      /* digits only, at most 5 per field so the uint16 range stays checkable */
      uint32_t v = 0;
      int digits = 0;
      while (*p >= '0' && *p <= '9') {
        v = v * 10U + (uint32_t)(*p - '0');
        p++;
        if (++digits > 5) {
          return false;
        }
      }
      if (digits == 0) {
        return false;
      }
      field[i] = v;
      if (i < 3) {
        if (*p != ':') {
          return false;
        }
        p++;
      }
    }
    if (field[0] > 247U || field[1] > 0xFFFFU || field[2] == 0U ||
        field[2] > 0xFFFFU || field[3] > 0xFFU) {
      return false;
    }
    if (n >= kMaxSegments) {
      return false;
    }
    tmp[n].slave_id = (uint8_t)field[0];
    tmp[n].start_address = (uint16_t)field[1];
    tmp[n].length = (uint16_t)field[2];
    tmp[n].read_command = (uint8_t)field[3];
    n++;
    if (*p == ';') {
      p++;
      continue;
    }
    break;
  }
  if (*p != '\0') {
    return false; /* trailing junk: a torn write, not a table */
  }
  for (int i = 0; i < n; i++) {
    segs_[i] = tmp[i];
  }
  count_ = n;
  source_ = Source::kDownloaded; /* only a download ever gets serialized */
  return true;
}

Source Table::Load(KeyValue& kv) {
  char buf[kMaxSegments * 24];
  if (kv.GetString(kTableKey, buf, sizeof(buf))) {
    if (Deserialize(buf)) {
      return source_;
    }
    kv.Erase(kTableKey); /* unreadable: do not let one bad write wedge collection */
  }
  const Segment* b = Builtin();
  int n = BuiltinCount();
  if (n > kMaxSegments) {
    n = kMaxSegments;
  }
  for (int i = 0; i < n; i++) {
    segs_[i] = b[i];
  }
  count_ = n;
  source_ = (n > 0) ? Source::kBuiltin : Source::kEmpty;
  return source_;
}

Apply Table::ApplyConfig(const char* payload, size_t len, KeyValue& kv) {
  Segment parsed[kMaxSegments];
  int n = 0;
  switch (ParseWire(payload, len, parsed, kMaxSegments, &n)) {
    case Parsed::kNoSegments:
      /* Not addressed to the point table (Switches.py sends "{}" for parity).
       * Keeping the current table is the only safe reading. */
      return Apply::kIgnored;
    case Parsed::kMalformed:
      return Apply::kRejected;
    case Parsed::kParsed:
      break;
  }

  Table next;

  if (n == 0) {
    /* {"segments":[]}: an explicit "do not poll anything", which overrides even the
     * segments of slaves this payload says nothing about - an empty table is a
     * statement about the bus, not about one slave. */
    next.source_ = Source::kDownloaded;
  } else {
    /* Per-unit merge: the slaves this payload mentions get their segments replaced
     * wholesale, everyone else keeps theirs. Without this, the second meter
     * Home Assistant configures wipes the first one off the bus. */
    uint8_t touched[kMaxSegments];
    int tn = 0;
    for (int i = 0; i < n; i++) {
      bool seen = false;
      for (int j = 0; j < tn; j++) {
        if (touched[j] == parsed[i].slave_id) {
          seen = true;
          break;
        }
      }
      if (!seen) {
        touched[tn++] = parsed[i].slave_id;
      }
    }
    for (int i = 0; i < count_; i++) {
      bool replace = false;
      for (int j = 0; j < tn; j++) {
        if (touched[j] == segs_[i].slave_id) {
          replace = true;
          break;
        }
      }
      if (!replace) {
        next.segs_[next.count_++] = segs_[i];
      }
    }
    for (int i = 0; i < n; i++) {
      if (next.count_ >= kMaxSegments) {
        return Apply::kRejected;  /* refuse, rather than drop a slave's points */
      }
      next.segs_[next.count_++] = parsed[i];
    }
    next.source_ = Source::kDownloaded;
  }

  if (next == *this) {
    /* HA re-sends config on every start; NVS is flash. Say so and write nothing. */
    if (source_ != Source::kDownloaded) {
      /* Same content but the provenance differs (a download confirms a built-in
       * default), so record that without rewriting the table itself. */
      source_ = Source::kDownloaded;
    }
    return Apply::kUnchanged;
  }

  *this = next;

  if (count_ == 0) {
    /* Nothing to record - but the stored table must go, or a reboot would
     * resurrect the one HA just cleared. */
    kv.Erase(kTableKey);
    return Apply::kUpdated;
  }

  char blob[kMaxSegments * 24];
  if (Serialize(blob, sizeof(blob)) == 0) {
    return Apply::kUpdatedVolatile; /* does not fit: nothing persisted */
  }
  if (!kv.SetString(kTableKey, blob)) {
    return Apply::kUpdatedVolatile;
  }
  return Apply::kUpdated;
}

}  // namespace registry
}  // namespace hcp
