#pragma once

#include "screen_power_policy.h"

#ifndef CROWPANEL_AWAKE_IDLE_POWER
#define CROWPANEL_AWAKE_IDLE_POWER 0
#endif

namespace AwakeIdle {
constexpr bool kEnabled = CROWPANEL_AWAKE_IDLE_POWER != 0 && !ScreenPower::kDynamicScreenPower;
constexpr uint32_t kTouchQuietMs = 5000;
constexpr uint32_t kWorkQuietMs = 2000;
constexpr uint32_t kTransitionQuietMs = 5000;
constexpr uint32_t kIdleTickMs = 10;

// Only CPLL /2: slower divider settings also reduce memory/bus clocks.
constexpr uint32_t idleCpuMHz(uint32_t full_mhz) {
  return full_mhz == 360 ? 180 : full_mhz == 400 ? 200 : full_mhz;
}

constexpr bool eligible(uint32_t now, uint32_t touch_at, uint32_t work_at,
                        uint32_t transition_at, bool awake, bool wake_pending,
                        bool network_safe, bool audio_idle) {
  return kEnabled && awake && !wake_pending && network_safe && audio_idle
      && uint32_t(now - touch_at) >= kTouchQuietMs
      && uint32_t(now - work_at) >= kWorkQuietMs
      && uint32_t(now - transition_at) >= kTransitionQuietMs;
}

constexpr uint32_t tickPeriodMs(bool awake, bool idle, uint32_t active_ms) {
  return ScreenPower::tickPeriodMs(awake, idle ? kIdleTickMs : active_ms);
}

constexpr uint32_t servicePeriodMs(uint32_t due_ms, bool idle, uint32_t active_ms) {
  const uint32_t maximum = idle ? 20 : 10;
  return due_ms < active_ms ? active_ms : due_ms > maximum ? maximum : due_ms;
}
}
