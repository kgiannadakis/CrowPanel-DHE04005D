#include "../../examples/crowpanel_p4_lvgl_chat/screen_power_policy.h"
using namespace ScreenPower;

static_assert(!retainedPauseUseful(false, true, true, true), "WiFi blocks CPU sleep: do not pause RGB");
static_assert(!retainedPauseUseful(true, false, true, true));
static_assert(!retainedPauseUseful(true, true, false, true));
static_assert(!retainedPauseUseful(true, true, true, false));
static_assert(retainedPauseUseful(true, true, true, true) == kRetainPanelForCpuSleep);

static_assert(!screenTimeoutDue(100, 101, 5000, true), "A concurrent wake must not underflow into sleep");
static_assert(!screenTimeoutDue(10000, 0, 5000, false), "Do not cancel a pending display wake");
static_assert(!screenTimeoutDue(5000, 0, 5000, true));
static_assert(screenTimeoutDue(5001, 0, 5000, true));
static_assert(screenTimeoutDue(100, UINT32_MAX - 5000, 5000, true), "Timeout across rollover");
static_assert(!wakeRedrawDue(true, false, 249, 0));
static_assert(wakeRedrawDue(true, false, 250, 0), "Retry a lost/failed first frame");
static_assert(!wakeRedrawDue(true, true, 250, 0));
static_assert(!wakeRedrawDue(false, false, 250, 0));
static_assert(!wakeRecoveryDue(true, 1499, 0));
static_assert(wakeRecoveryDue(true, 1500, 0), "Bound a stalled scan/resume wait");
static_assert(!wakeRecoveryDue(false, 1500, 0));
static_assert(wakeRecoveryDue(true, 1499, UINT32_MAX), "Recovery across rollover");
static_assert(touchRequestedWake(true, true, false, true));
static_assert(touchRequestedWake(true, false, true, false), "Keep a short touch GPIO wake");
static_assert(!touchRequestedWake(true, false, true, true), "Radio alone is not a touch");
static_assert(!touchRequestedWake(false, true, true, false));
static_assert(!wakeReady(true, false, 5000, 100), "Recovery still needs a clean frame");
static_assert(!wakeReady(true, true, 5000, 1), "Recovery still needs two scan frames");
