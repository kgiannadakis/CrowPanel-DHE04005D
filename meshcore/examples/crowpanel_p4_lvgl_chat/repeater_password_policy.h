#pragma once
#include <stddef.h>
#include <stdint.h>

namespace RepeaterPassword {
constexpr size_t kMaxBytes = 15;  // MeshCore sendLogin() limit, not UTF-8 characters.
constexpr size_t kBufferBytes = kMaxBytes + 1;
constexpr size_t kPathBytes = sizeof("/rptpw_") + 64;
struct Record {
  uint8_t version = 1;
  char password[kBufferBytes] = {};
};
constexpr bool valid(const Record& record) {
  return record.version == 1 && record.password[kMaxBytes] == '\0';
}
constexpr bool canReveal(bool using_saved, bool has_typed_text, bool show_checked) {
  return !using_saved && has_typed_text && show_checked;
}
constexpr bool makePath(const uint8_t* key, char* out, size_t capacity) {
  if (!key || !out || capacity < kPathBytes) return false;
  constexpr char prefix[] = "/rptpw_";
  constexpr char hex[] = "0123456789abcdef";
  for (size_t i = 0; i < sizeof(prefix) - 1; ++i) out[i] = prefix[i];
  for (size_t i = 0; i < 32; ++i) {
    out[sizeof(prefix) - 1 + i * 2] = hex[key[i] >> 4];
    out[sizeof(prefix) + i * 2] = hex[key[i] & 15];
  }
  out[kPathBytes - 1] = '\0';
  return true;
}
}
