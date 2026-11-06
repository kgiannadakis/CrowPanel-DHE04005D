#include "../../examples/crowpanel_p4_lvgl_chat/awake_idle_policy.h"

static_assert(AwakeIdle::idleCpuMHz(360) == 180);
static_assert(AwakeIdle::idleCpuMHz(400) == 200);
static_assert(AwakeIdle::idleCpuMHz(180) == 180);
static_assert(AwakeIdle::idleCpuMHz(90) == 90);
static_assert(AwakeIdle::eligible(5000, 0, 3000, 0, true, false, true, true) == AwakeIdle::kEnabled);
static_assert(!AwakeIdle::eligible(4999, 0, 0, 0, true, false, true, true));
static_assert(!AwakeIdle::eligible(5000, 1, 0, 0, true, false, true, true));
static_assert(!AwakeIdle::eligible(5000, 0, 3001, 0, true, false, true, true));
static_assert(!AwakeIdle::eligible(5000, 0, 0, 1, true, false, true, true));
static_assert(!AwakeIdle::eligible(5000, 0, 0, 0, false, false, true, true));
static_assert(!AwakeIdle::eligible(5000, 0, 0, 0, true, true, true, true));
static_assert(!AwakeIdle::eligible(5000, 0, 0, 0, true, false, false, true));
static_assert(!AwakeIdle::eligible(5000, 0, 0, 0, true, false, true, false));
static_assert(AwakeIdle::eligible(4999, UINT32_MAX, UINT32_MAX, UINT32_MAX,
                                true, false, true, true) == AwakeIdle::kEnabled);
static_assert(AwakeIdle::tickPeriodMs(true, false, 2) == 2);
static_assert(AwakeIdle::tickPeriodMs(true, true, 2) == 10);
static_assert(AwakeIdle::tickPeriodMs(false, false, 2) ==
              (ScreenPower::kBacklightOnlySleep ? 2 : ScreenPower::kDarkTickPeriodMs));
static_assert(AwakeIdle::servicePeriodMs(100, false, 2) == 10);
static_assert(AwakeIdle::servicePeriodMs(100, true, 2) == 20);
static_assert(AwakeIdle::servicePeriodMs(5, true, 2) == 5);
static_assert(AwakeIdle::servicePeriodMs(0, true, 2) == 2);
static_assert(ScreenPower::cpuClockMHz(360, false) ==
              (ScreenPower::kDynamicScreenPower ? 180 : 360));
