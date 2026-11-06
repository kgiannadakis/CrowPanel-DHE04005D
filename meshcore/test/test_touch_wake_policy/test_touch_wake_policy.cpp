#include "../../variants/crowpanel_dhe04005d/gt911_wake_policy.h"

using namespace TouchWake;

static_assert(activeLevel(0) == 1, "Rising edge uses active-high GPIO wake");
static_assert(activeLevel(1) == 0, "Falling edge uses active-low GPIO wake");
static_assert(activeLevel(2) == 0, "Low-level configuration stays active-low");
static_assert(activeLevel(3) == 1, "High-level configuration stays active-high");
static_assert(activeLevel(0xA1) == 0, "Ignore unrelated panel configuration bits");

constexpr bool verifyBothPolarities() {
  for (int active = 0; active <= 1; ++active) {
    Verification v;
    if (v.observe(false, 1 - active, active)) return false;
    if (v.observe(true, 1 - active, active)) return false;
    if (!v.observe(true, active, active)) return false;
    if (!v.observe(false, 1 - active, active)) return false;
    v.reset();
    if (v.observe(true, active, active)) return false;
    if (!v.observe(false, 1 - active, active)) return false;
  }
  return true;
}
static_assert(verifyBothPolarities(), "Require an idle level and a genuine matching touch");

constexpr bool rejectUnverifiedInputs() {
  Verification v;
  if (v.observe(true, 1, -1) || v.observe(false, 0, -1)) return false;
  if (v.observe(true, 2, 1) || v.observe(false, -1, 1)) return false;
  for (int i = 0; i < 20; ++i)
    if (v.observe(false, 1, 1)) return false;
  return !v.observe(true, 1, 1);
}
static_assert(rejectUnverifiedInputs(), "A floating/stuck active pin alone cannot qualify");

constexpr bool revokeStuckPin() {
  Verification v;
  v.observe(false, 0, 1);
  if (!v.observe(true, 1, 1)) return false;
  if (!v.observe(false, 1, 1, true)) return false;
  if (!v.observe(false, 1, 1, true)) return false;
  if (v.observe(false, 1, 1, true)) return false;
  if (v.observe(false, 0, 1)) return false;
  return v.observe(true, 1, 1);
}
static_assert(revokeStuckPin(), "Fall back after stuck-active empty-buffer samples");

int main() { return 0; }
