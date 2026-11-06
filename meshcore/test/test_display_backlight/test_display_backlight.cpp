#include "../../examples/crowpanel_p4_lvgl_chat/backlight_write_policy.h"
using namespace Backlight;

static_assert(clampLevel(0) == 0 && clampLevel(1) == 5);
static_assert(clampLevel(5) == 5 && clampLevel(100) == 100 && clampLevel(255) == 100);

constexpr bool wake_and_sleep() {
  Writes w;
  if (w.offConfirmed() || w.due(5, false, 0) || !w.due(0, false, 0)) return false;
  w.record(0, true, 0);
  if (!w.offConfirmed() || w.due(0, false, 1) || w.due(5, false, 1)) return false;
  if (!w.due(5, true, 1)) return false;
  w.record(5, true, 1);
  if (w.applied() != 5 || w.offConfirmed() || w.due(5, true, 2)) return false;
  if (!w.due(0, false, 2)) return false;
  w.record(0, true, 2);
  return w.offConfirmed() && w.due(5, true, 3);
}
static_assert(wake_and_sleep(), "Restore only after a valid frame; off does not wait for a frame");

constexpr bool failed_writes_retry() {
  Writes w;
  w.record(0, true, 0);
  w.record(5, false, 10);
  if (w.offConfirmed() || w.applied() != -1 || w.due(5, true, 1009)) return false;
  if (!w.due(5, true, 1010) || w.due(5, false, 1010)) return false;
  // A newer sleep/brightness request must bypass the old request's retry delay.
  if (!w.due(0, false, 11) || !w.due(50, true, 11)) return false;
  w.record(0, false, 11);
  if (w.offConfirmed() || w.due(0, false, 1010)) return false;
  w.record(0, true, 1011);
  return w.offConfirmed() && !w.due(0, false, 1012);
}
static_assert(failed_writes_retry(), "A failed PWM write must not finish wake or allow CPU sleep");

constexpr bool retry_wraps() {
  Writes w;
  w.record(5, false, UINT32_MAX - 499);
  return !w.due(5, true, 499) && w.due(5, true, 500);
}
static_assert(retry_wraps());
