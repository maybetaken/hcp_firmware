/*
 * HCP storage abstraction — a small key/value store.
 * Replaces the legacy ESP-IDF NVS usage (serial number, config blobs).
 * Backend selected at build time: Zephyr `settings` on-target, filesystem on
 * host. Google C++ Style naming.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace hcp {
namespace storage {

class Store {
 public:
  Store();
  ~Store();
  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;

  // Prepare the store. On posix `root` is a directory (nullptr -> default);
  // on Zephyr it maps to the settings subsys. Returns false on failure.
  bool Open(const char* root = nullptr);
  bool Close();

  bool SetString(const char* key, const char* val);
  bool GetString(const char* key, char* out, size_t cap,
                 size_t* out_len = nullptr);

  bool SetInt(const char* key, int32_t val);
  bool GetInt(const char* key, int32_t& val);

  bool Exists(const char* key);
  bool Erase(const char* key);

 private:
  struct Impl;
  Impl* impl_;
};

}  // namespace storage
}  // namespace hcp
