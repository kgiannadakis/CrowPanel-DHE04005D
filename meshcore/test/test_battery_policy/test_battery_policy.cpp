#include "../../examples/crowpanel_p4_lvgl_chat/battery_policy.h"

using namespace Battery;

static_assert(percentFromMillivolts(4200) == 100, "Clamp above full");
static_assert(percentFromMillivolts(4100) == 100, "Preserve the existing curve");
static_assert(percentFromMillivolts(3750) == 55, "Preserve midpoint calibration");
static_assert(percentFromMillivolts(3725) == 48, "Interpolate between curve points");
static_assert(percentFromMillivolts(3100) == 0, "Preserve the empty point");
static_assert(percentFromMillivolts(2500) == 0, "Clamp below empty");

constexpr bool invalidReadings() {
  Estimate e;
  if (e.haveReading() || e.sample(0, false, 0) || e.sample(5001, false, 0)) return false;
  if (!e.sample(3750, false, 0) || e.percent() != 55) return false;
  return !e.sample(2499, true, 5000) && e.percent() == 55;
}
static_assert(invalidReadings(), "Bad I2C data must not change the estimate");

constexpr bool dischargeRebound() {
  Estimate e;
  e.sample(3750, false, 0);
  e.observeScreen(false, 1000);
  for (uint32_t t = 5000; t <= 300000; t += kPollMs)
    e.sample(4000, false, t);
  if (e.percent() != 55) return false;
  e.observeScreen(true, 301000);
  for (uint32_t t = 305000; t <= 320000; t += kPollMs)
    e.sample(4000, false, t);
  for (uint32_t t = 325000; t <= 400000; t += kPollMs)
    e.sample(3750, false, t);
  return e.percent() == 55;
}
static_assert(dischargeRebound(), "Dark and wake recovery must not invent charge");

constexpr bool isolatedSag() {
  Estimate e;
  e.sample(3800, false, 0);
  for (uint32_t t = 5000; t <= 60000; t += kPollMs)
    e.sample(t == 15000 || t == 40000 ? 3300 : 3800, false, t);
  return e.percent() == 68;
}
static_assert(isolatedSag(), "Median must reject isolated TX/ADC dips");

constexpr bool prolongedDischarge() {
  Estimate e;
  e.sample(3800, false, 0);
  e.observeScreen(false, 1000);
  for (uint32_t t = 5000; t <= 300000; t += kPollMs) {
    const int before = e.percent();
    e.sample(3600, false, t);
    if (e.percent() > before || before - e.percent() > 2) return false;
  }
  return e.percent() >= 25 && e.percent() <= 26;
}
static_assert(prolongedDischarge(), "Long sleeps must still account for discharge");

constexpr bool chargingAndWake() {
  Estimate e;
  e.sample(3750, false, 0);
  e.observeScreen(false, 1000);
  for (uint32_t t = 5000; t <= 120000; t += kPollMs)
    e.sample(4100, true, t);
  if (e.percent() != 55) return false;
  e.observeScreen(true, 121000);
  for (uint32_t t = 125000; t <= 135000; t += kPollMs)
    e.sample(4100, true, t);
  if (e.percent() != 55) return false;
  for (uint32_t t = 140000; t <= 300000; t += kPollMs) {
    const int before = e.percent();
    e.sample(4100, true, t);
    if (e.percent() < before || e.percent() - before > 2) return false;
  }
  return e.percent() == 100;
}
static_assert(chargingAndWake(), "Real charging must resume smoothly after waking");

constexpr bool unplugAndWrap() {
  Estimate e;
  e.sample(3800, true, UINT32_MAX - 10000);
  e.sample(3400, false, UINT32_MAX - 5000);
  e.sample(3400, false, 5000);  // 10001 ms since unplug
  if (e.percent() != 68) return false;
  e.sample(3400, false, 10000); // settling expires across millis wrap
  e.sample(3400, false, 15000);
  e.sample(3400, false, 20000);
  return e.percent() == 66;
}
static_assert(unplugAndWrap(), "Charger settling must handle timer wrap");

constexpr bool jitterAndBounds() {
  Estimate e;
  e.sample(3750, false, 0);
  for (uint32_t t = 5000; t <= 60000; t += kPollMs)
    e.sample(t % 10000 ? 3749 : 3751, false, t);
  if (e.percent() != 55) return false;
  for (uint32_t t = 65000; t <= 600000; t += kPollMs) {
    e.sample(2500, false, t);
    if (e.percent() < 0 || e.percent() > 100) return false;
  }
  return e.percent() == 0;
}
static_assert(jitterAndBounds(), "Suppress one-percent jitter and reach genuine empty");

int main() { return 0; }
