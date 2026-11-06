#include "../../examples/crowpanel_p4_lvgl_chat/repeater_password_policy.h"
using namespace RepeaterPassword;

static_assert(kMaxBytes == 15);
static_assert(sizeof(Record) == 17);
static_assert(valid(Record{}), "An explicitly saved empty password is valid");
constexpr bool rejectCorruptRecord() {
  Record record;
  record.version = 2;
  if (valid(record)) return false;
  record.version = 1;
  record.password[kMaxBytes] = 'x';
  return !valid(record);
}
static_assert(rejectCorruptRecord());

constexpr bool fullIdentityPaths() {
  uint8_t first[32] = {}, second[32] = {};
  second[31] = 0xab;
  char a[kPathBytes] = {}, b[kPathBytes] = {};
  if (!makePath(first, a, sizeof(a)) || !makePath(second, b, sizeof(b))) return false;
  if (a[kPathBytes - 1] != 0 || b[kPathBytes - 1] != 0) return false;
  for (size_t i = 0; i < kPathBytes - 3; ++i) if (a[i] != b[i]) return false;
  return a[kPathBytes - 3] == '0' && a[kPathBytes - 2] == '0'
      && b[kPathBytes - 3] == 'a' && b[kPathBytes - 2] == 'b'
      && !makePath(nullptr, a, sizeof(a))
      && !makePath(first, nullptr, sizeof(a))
      && !makePath(first, a, sizeof(a) - 1);
}
static_assert(fullIdentityPaths(), "Use every identity byte and reject undersized buffers");
static_assert(!canReveal(true, true, true), "Saved secrets must never be revealed");
static_assert(!canReveal(true, false, true));
static_assert(!canReveal(false, false, true));
static_assert(!canReveal(false, true, false));
static_assert(canReveal(false, true, true), "Only newly typed text can be shown");
