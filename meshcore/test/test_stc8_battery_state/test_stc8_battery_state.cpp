#include "../../variants/crowpanel_dhe04005d/stc8_battery_state.h"
#include "../../examples/crowpanel_p4_lvgl_chat/battery_policy.h"

static_assert(STC8_BAT_IDLE == 0);
static_assert(STC8_BAT_CHARGING == 1);
static_assert(STC8_BAT_FULLY_CHARGED == 2);
static_assert(STC8_BAT_DISCHARGING == 3);
static_assert(STC8_BAT_ERROR == 4);
static_assert(!Stc8Battery::isCharging(STC8_BAT_IDLE));
static_assert(Stc8Battery::isCharging(STC8_BAT_CHARGING));
static_assert(Stc8Battery::isCharging(STC8_BAT_FULLY_CHARGED));
static_assert(!Stc8Battery::isCharging(STC8_BAT_DISCHARGING));
static_assert(!Stc8Battery::isCharging(STC8_BAT_ERROR));
static_assert(!Stc8Battery::isCharging(255));
static_assert(Battery::pollPeriodMs(false) == 1000);
static_assert(Battery::pollPeriodMs(true) == 5000);

constexpr bool battery_only_cold_boot() {
  Battery::Estimate estimate;
  if (estimate.haveReading() || estimate.sample(0, false, 0)) return false;
  if (!estimate.sample(3800, Stc8Battery::isCharging(3), 1000)
      || !estimate.haveReading() || estimate.percent() != 68) return false;
  for (uint32_t now = 6000; now <= 31000; now += 5000)
    if (!estimate.sample(4200, Stc8Battery::isCharging(3), now)) return false;
  return estimate.percent() == 68;  // Discharging cannot raise the estimate.
}
static_assert(battery_only_cold_boot());
