/*
 * hcp::storage — Zephyr backend.
 *
 * In-RAM cache (source of truth for reads) + persistence via the Zephyr
 * `settings` subsys. Open() registers handler "hcp" and settings_load()
 * (h_set repopulates the cache); SetString caches + settings_save_one();
 * Erase drops cache + settings_delete(). Values are stored as strings,
 * matching the POSIX backend. Google C++ Style naming.
 *
 * Copyright (c) 2026 maybetaken
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "hcp/storage/store.h"

#include <zephyr/settings/settings.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace hcp {
namespace storage {

namespace {

constexpr const char* kPrefix = "hcp";

#ifndef CONFIG_HCP_STORAGE_MAX_KEYS
#define CONFIG_HCP_STORAGE_MAX_KEYS 16
#endif
#ifndef CONFIG_HCP_STORAGE_MAX_VALUE
#define CONFIG_HCP_STORAGE_MAX_VALUE 64
#endif
#ifndef CONFIG_HCP_STORAGE_KEY_MAX
#define CONFIG_HCP_STORAGE_KEY_MAX 16
#endif

struct Entry {
  bool used = false;
  char key[CONFIG_HCP_STORAGE_KEY_MAX];
  uint8_t val[CONFIG_HCP_STORAGE_MAX_VALUE];
  uint16_t len;
};

Entry g_cache[CONFIG_HCP_STORAGE_MAX_KEYS];

// Strip a leading '/' from a settings key (handler prefix already removed).
const char* NormKey(const char* key) {
  while (*key == '/') {
    key++;
  }
  return key;
}

Entry* FindEntry(const char* key) {
  for (auto& e : g_cache) {
    if (e.used && strcmp(e.key, key) == 0) {
      return &e;
    }
  }
  return nullptr;
}

Entry* AllocEntry(const char* key) {
  Entry* e = FindEntry(key);
  if (e) {
    return e;
  }
  for (auto& slot : g_cache) {
    if (!slot.used) {
      slot.used = true;
      slot.len = 0;
      strncpy(slot.key, key, CONFIG_HCP_STORAGE_KEY_MAX - 1);
      slot.key[CONFIG_HCP_STORAGE_KEY_MAX - 1] = '\0';
      return &slot;
    }
  }
  return nullptr;  // full
}

void FreeEntry(Entry* e) {
  if (e) {
    e->used = false;
    e->len = 0;
  }
}

// settings h_set: called during settings_load() for each stored "hcp/<key>".
int OnSet(const char* key, size_t len, settings_read_cb read_cb, void* cb_arg) {
  const char* k = NormKey(key);
  if (len > CONFIG_HCP_STORAGE_MAX_VALUE) {
    return 0;
  }
  Entry* e = AllocEntry(k);
  if (!e) {
    return 0;
  }
  int r = read_cb(cb_arg, e->val, len);
  if (r < 0) {
    FreeEntry(e);
    return r;
  }
  e->len = static_cast<uint16_t>(r > 0 ? r : static_cast<int>(len));
  return 0;
}

struct settings_handler g_conf = {
    .name = kPrefix,
    .h_set = OnSet,
    .h_commit = nullptr,
};

void BuildName(char* out, size_t cap, const char* key) {
  snprintf(out, cap, "%s/%s", kPrefix, key);
}

}  // namespace

struct Store::Impl {
  bool open = false;
};

static bool g_registered = false;

Store::Store() : impl_(new Impl()) {}
Store::~Store() { delete impl_; }

bool Store::Open(const char* /*root*/) {
  if (impl_->open) {
    return true;
  }
  // Entry has default member initializers, so reset it by assignment rather
  // than memset() (which warns for non-trivial types).
  for (auto& e : g_cache) {
    e = Entry{};
  }
  if (settings_subsys_init() != 0) {
    return false;
  }
  if (!g_registered) {
    if (settings_register(&g_conf) != 0) {
      return false;
    }
    g_registered = true;
  }
  if (settings_load() != 0) {
    return false;
  }
  impl_->open = true;
  return true;
}

bool Store::Close() {
  impl_->open = false;
  return true;
}

bool Store::SetString(const char* key, const char* val) {
  const size_t len = strlen(val);
  if (len > CONFIG_HCP_STORAGE_MAX_VALUE) {
    return false;
  }
  Entry* e = AllocEntry(key);
  if (!e) {
    return false;  // cache full
  }
  memcpy(e->val, val, len);
  e->len = static_cast<uint16_t>(len);

  char name[CONFIG_HCP_STORAGE_KEY_MAX + 8];
  BuildName(name, sizeof(name), key);
  // Cache is the in-session source of truth; persistence is best-effort.
  (void)settings_save_one(name, val, len);
  return true;
}

bool Store::GetString(const char* key, char* out, size_t cap, size_t* out_len) {
  Entry* e = FindEntry(key);
  if (!e || cap == 0) {
    return false;
  }
  size_t n = e->len;
  if (n > cap - 1) {
    n = cap - 1;
  }
  memcpy(out, e->val, n);
  out[n] = '\0';
  if (out_len) {
    *out_len = n + 1;
  }
  return true;
}

bool Store::SetInt(const char* key, int32_t val) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%d", static_cast<int>(val));
  return SetString(key, buf);
}

bool Store::GetInt(const char* key, int32_t& val) {
  char buf[16];
  if (!GetString(key, buf, sizeof(buf))) {
    return false;
  }
  val = static_cast<int32_t>(strtol(buf, nullptr, 10));
  return true;
}

bool Store::Exists(const char* key) { return FindEntry(key) != nullptr; }

bool Store::Erase(const char* key) {
  FreeEntry(FindEntry(key));
  char name[CONFIG_HCP_STORAGE_KEY_MAX + 8];
  BuildName(name, sizeof(name), key);
  settings_delete(name);
  return true;
}

}  // namespace storage
}  // namespace hcp
