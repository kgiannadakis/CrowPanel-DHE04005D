#include "../../examples/crowpanel_p4_lvgl_chat/awake_idle_policy.h"

static_assert(!ScreenPower::kCpuLightSleep, "Do not enter CPU light sleep in the stable default");
static_assert(!ScreenPower::kDynamicScreenPower, "Do not change LCD/CPU clocks on screen-off");
static_assert(!ScreenPower::kSuspendRgbWhenDark, "Do not tear down the RGB panel");
static_assert(!ScreenPower::kRetainPanelForCpuSleep, "Do not pause RGB DMA");
static_assert(!ScreenPower::kTouchIrqSleep, "Do not reconfigure touch GPIO for sleep");
static_assert(!AwakeIdle::kEnabled, "Keep the CPU at its boot frequency");
static_assert(ScreenPower::kBacklightOnlySleep);
static_assert(!ScreenPower::kDisplayTaskBacklight, "Keep the known-good backlight owner unchanged");
static_assert(ScreenPower::cpuClockMHz(360, false) == 360);
static_assert(ScreenPower::pixelClockHz(18000000, false) == 18000000);
static_assert(ScreenPower::tickPeriodMs(false, 2) == 2);
static_assert(AwakeIdle::tickPeriodMs(false, false, 2) == 2);
static_assert(ScreenPower::savedBrightness(5) == 5, "Keep minimum brightness persistence");
