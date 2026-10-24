/*
 * hcp::switchctl - see command.h for what these shapes are and which side of the
 * contract each one belongs to.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "hcp/switch/command.h"

#include <stdio.h>
#include <string.h>

namespace hcp {
namespace switchctl {
namespace {

constexpr int kMinAnnouncedSwitches = 2;  // Switches.MIN_SWITCH_COUNT

bool IsSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

void SkipSpace(const char* b, size_t n, size_t* i) {
  while (*i < n && IsSpace(b[*i])) {
    (*i)++;
  }
}

/*
 * Read a JSON string, leaving `i` past the closing quote.
 *
 * Returns false for anything that is not a plain quoted string, including one
 * carrying a backslash escape. That is deliberate rather than lazy: every key and
 * value in this contract is a bare ASCII word, so a payload with an escape is not a
 * command this device was asked to run, and half-interpreting it (which is what
 * decoding \" naively does) is how a relay ends up moving on a message that meant
 * something else.
 */
bool ReadString(const char* b, size_t n, size_t* i, size_t* start, size_t* len) {
  if (*i >= n || b[*i] != '"') {
    return false;
  }
  size_t j = *i + 1;
  const size_t from = j;
  while (j < n && b[j] != '"') {
    if (b[j] == '\\') {
      return false;
    }
    j++;
  }
  if (j >= n) {
    return false;  // unterminated: the payload was cut
  }
  *start = from;
  *len = j - from;
  *i = j + 1;
  return true;
}

/* "switch_01" / "switch_1" -> index 1. Zero is not addressable. */
bool KeyToIndex(const char* b, size_t start, size_t len, uint8_t* index) {
  if (len <= 7 || memcmp(b + start, "switch_", 7) != 0) {
    return false;
  }
  unsigned value = 0;
  int digits = 0;
  for (size_t i = 7; i < len; i++) {
    const char c = b[start + i];
    if (c < '0' || c > '9') {
      return false;
    }
    value = value * 10U + static_cast<unsigned>(c - '0');
    if (value > 255U) {  // does not fit the index this type carries
      return false;
    }
    digits++;
  }
  if (digits == 0 || value == 0U) {
    return false;
  }
  *index = static_cast<uint8_t>(value);
  return true;
}

bool StateOf(const char* b, size_t start, size_t len, bool* on) {
  if (len == 2 && memcmp(b + start, "on", 2) == 0) {
    *on = true;
    return true;
  }
  if (len == 3 && memcmp(b + start, "off", 3) == 0) {
    *on = false;
    return true;
  }
  return false;
}

}  // namespace

int ParseCommands(const uint8_t* buf, size_t len, SwitchCommand* out, int max) {
  if (buf == nullptr || out == nullptr || max <= 0) {
    return 0;
  }
  const char* b = reinterpret_cast<const char*>(buf);
  size_t i = 0;
  SkipSpace(b, len, &i);
  if (i >= len || b[i] != '{') {
    return 0;
  }
  i++;

  int count = 0;
  while (true) {
    SkipSpace(b, len, &i);
    if (i >= len) {
      /* The object never closed, so this payload arrived in pieces. Applying the part
       * that did parse is the wrong call: a command that sets two relays and is cut
       * after the first leaves the load in a state nobody asked for, and the sender
       * has no way to learn that. Nothing is better than half something. */
      return 0;
    }
    if (b[i] == '}') {
      return count;
    }

    size_t kstart = 0;
    size_t klen = 0;
    if (!ReadString(b, len, &i, &kstart, &klen)) {
      return count;
    }
    SkipSpace(b, len, &i);
    if (i >= len || b[i] != ':') {
      return count;
    }
    i++;
    SkipSpace(b, len, &i);

    size_t vstart = 0;
    size_t vlen = 0;
    if (ReadString(b, len, &i, &vstart, &vlen)) {
      uint8_t index = 0;
      bool on = false;
      if (KeyToIndex(b, kstart, klen, &index) && StateOf(b, vstart, vlen, &on)) {
        if (count == max) {
          return count;  // full: the caller sees max and knows there may be more
        }
        out[count].index = index;
        out[count].on = on;
        count++;
      }
    } else {
      // A number, null or nested value: skip it and keep looking at the rest.
      while (i < len && b[i] != ',' && b[i] != '}') {
        i++;
      }
    }

    SkipSpace(b, len, &i);
    if (i >= len) {
      return 0;  // same reasoning as the loop top
    }
    if (b[i] == ',') {
      i++;
      continue;
    }
    if (b[i] == '}') {
      return count;
    }
    return count;  // malformed separator
  }
}

int FormatNotify(const bool* states, int count, char* buf, size_t cap) {
  if (states == nullptr || buf == nullptr || count <= 0) {
    return 0;
  }
  size_t used = 0;
  buf[used++] = '{';
  for (int i = 0; i < count; i++) {
    char piece[24];
    const int n = snprintf(piece, sizeof(piece), "%s\"switch_%02d\":\"%s\"",
                           i == 0 ? "" : ",", i + 1, states[i] ? "on" : "off");
    /* One byte has to stay for the closing brace, and a document without it is not
     * JSON - Home Assistant's parse fails and the device reads as dead rather than
     * as stale, so a refusal here is strictly better than a partial publish. */
    if (n <= 0 || used + static_cast<size_t>(n) + 1 > cap) {
      return 0;
    }
    memcpy(buf + used, piece, static_cast<size_t>(n));
    used += static_cast<size_t>(n);
  }
  buf[used++] = '}';
  return static_cast<int>(used);
}

int FormatOnline(int switch_count, const char* version, char* buf, size_t cap) {
  if (buf == nullptr || version == nullptr ||
      switch_count < kMinAnnouncedSwitches) {
    return 0;
  }
  /* The spelling is the previous firmware's (main/solar_manager.cpp:196), with spaces
   * included: it is what the integration has been given in the field, and while
   * json.loads does not care, keeping the bytes identical means a device that behaves
   * differently is a firmware difference and not a formatting one. */
  const int n = snprintf(buf, cap, "{\"switch_count\": %d, \"version\": \"%s\"}",
                         switch_count, version);
  if (n <= 0 || static_cast<size_t>(n) >= cap) {
    return 0;
  }
  return n;
}

}  // namespace switchctl
}  // namespace hcp
