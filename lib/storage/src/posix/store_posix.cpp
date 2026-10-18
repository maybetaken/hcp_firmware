/*
 * hcp::storage — POSIX backend (host). One file per key under a root dir.
 * Google C++ Style naming.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "hcp/storage/store.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace hcp {
namespace storage {

struct Store::Impl {
  std::string root;
};

static std::string PathFor(const std::string& root, const char* key) {
  return root + "/" + key;
}

Store::Store() : impl_(new Impl()) {}
Store::~Store() { delete impl_; }

bool Store::Open(const char* root) {
  if (root) {
    impl_->root = root;
  } else {
    const char* env = getenv("HCP_STORAGE_DIR");
    impl_->root = env ? env : "./hcp_store";
  }
  if (mkdir(impl_->root.c_str(), 0775) != 0 && errno != EEXIST) {
    return false;
  }
  return true;
}

bool Store::Close() { return true; }

bool Store::SetString(const char* key, const char* val) {
  std::string p = PathFor(impl_->root, key);
  FILE* f = fopen(p.c_str(), "wb");
  if (!f) {
    return false;
  }
  fwrite(val, 1, strlen(val), f);
  fclose(f);
  return true;
}

bool Store::GetString(const char* key, char* out, size_t cap, size_t* out_len) {
  if (cap == 0) {
    return false;
  }
  std::string p = PathFor(impl_->root, key);
  FILE* f = fopen(p.c_str(), "rb");
  if (!f) {
    return false;
  }
  size_t r = fread(out, 1, cap - 1, f);
  fclose(f);
  out[r] = '\0';
  if (out_len) {
    *out_len = r + 1;
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

bool Store::Exists(const char* key) {
  struct stat st;
  std::string p = PathFor(impl_->root, key);
  return stat(p.c_str(), &st) == 0;
}

bool Store::Erase(const char* key) {
  std::string p = PathFor(impl_->root, key);
  return remove(p.c_str()) == 0;
}

}  // namespace storage
}  // namespace hcp
