#pragma once

#include <stdint.h>

namespace ScreenPower {
constexpr uint8_t kBacklightOff = 0;
constexpr uint8_t kMinBacklight = 5;
constexpr uint8_t kMaxBacklight = 100;
constexpr uint8_t kRampStep = 10;
constexpr uint32_t kDarkUiPeriodMs = 80;
constexpr uint32_t kDarkTickPeriodMs = 80;
constexpr uint32_t kWakeSettleMs = 120;
constexpr uint32_t kWakeScanFrames = 2;
constexpr uint32_t kPanelRetryMs = 1000;
constexpr uint32_t kWakeRedrawRetryMs = 250;
constexpr uint32_t kWakeRecoveryMs = 1500;
#ifndef CROWPANEL_CPU_SLEEP_MS
#define CROWPANEL_CPU_SLEEP_MS 80
#endif
constexpr uint32_t kCpuSleepMs = CROWPANEL_CPU_SLEEP_MS;
static_assert(kCpuSleepMs >= 20 && kCpuSleepMs <= kDarkUiPeriodMs,
              "Keep timer-only touch and application servicing within 80 ms");
#ifndef CROWPANEL_TOUCH_IRQ_SLEEP
#define CROWPANEL_TOUCH_IRQ_SLEEP 0
#endif
#ifndef CROWPANEL_TOUCH_IRQ_SLEEP_MS
#define CROWPANEL_TOUCH_IRQ_SLEEP_MS 250
#endif
constexpr bool kTouchIrqSleep = CROWPANEL_TOUCH_IRQ_SLEEP != 0;
constexpr uint32_t kTouchIrqSleepMs = CROWPANEL_TOUCH_IRQ_SLEEP_MS;
static_assert(kTouchIrqSleepMs >= 80 && kTouchIrqSleepMs <= 500,
              "Bound background servicing even with interrupt-driven touch");
constexpr uint32_t kScanPauseSettleMs = 60;
constexpr uint32_t kScanPauseTimeoutMs = 250;

// Safe fallback when build flags are absent. The normal PlatformIO environment
// explicitly selects the user-tested CPU sleep and retained-panel settings.
#ifndef CROWPANEL_DYNAMIC_SCREEN_POWER
#define CROWPANEL_DYNAMIC_SCREEN_POWER 0
#endif
constexpr bool kDynamicScreenPower = CROWPANEL_DYNAMIC_SCREEN_POWER != 0;

#ifndef CROWPANEL_CPU_LIGHT_SLEEP
#define CROWPANEL_CPU_LIGHT_SLEEP 0
#endif
constexpr bool kCpuLightSleep = CROWPANEL_CPU_LIGHT_SLEEP != 0;
constexpr bool kRetainPanelForCpuSleep = kCpuLightSleep && !kDynamicScreenPower;
constexpr bool kBacklightOnlySleep = !kCpuLightSleep && !kDynamicScreenPower;

#ifndef CROWPANEL_DISPLAY_TASK_BACKLIGHT
#define CROWPANEL_DISPLAY_TASK_BACKLIGHT 0
#endif
constexpr bool kDisplayTaskBacklight = CROWPANEL_DISPLAY_TASK_BACKLIGHT != 0;
static_assert(!kDisplayTaskBacklight || kRetainPanelForCpuSleep,
              "Display-owned backlight requires retained-panel CPU sleep");

constexpr uint32_t cpuSleepPeriodMs(bool touch_verified) {
  return kCpuLightSleep && kTouchIrqSleep && touch_verified ? kTouchIrqSleepMs : kCpuSleepMs;
}

constexpr bool wakeInputsQuiet(bool radio_pending, bool touch_armed,
                               bool touch_active, bool screen_awake) {
  return !radio_pending && !(touch_armed && touch_active) && !screen_awake;
}

constexpr uint32_t tickPeriodMs(bool screen_awake, uint32_t active_ms) {
  return screen_awake || kBacklightOnlySleep ? active_ms : kDarkTickPeriodMs;
}

constexpr bool touchRequestedWake(bool touch_armed, bool touch_pending,
                                   bool gpio_wake, bool radio_pending) {
  return touch_armed && (touch_pending || (gpio_wake && !radio_pending));
}

constexpr bool screenTimeoutDue(uint32_t now, uint32_t last_activity,
                                 uint32_t timeout_ms, bool backlight_ready) {
  // Another task can publish a newer touch timestamp after now was read.
  const int32_t elapsed = static_cast<int32_t>(now - last_activity);
  return backlight_ready && timeout_ms >= 5000 && elapsed >= 0
      && static_cast<uint32_t>(elapsed) > timeout_ms;
}

constexpr bool wakeRedrawDue(bool pending, bool frame_rendered,
                              uint32_t now, uint32_t last_redraw) {
  return pending && !frame_rendered && uint32_t(now - last_redraw) >= kWakeRedrawRetryMs;
}

constexpr bool wakeRecoveryDue(bool pending, uint32_t now, uint32_t last_recovery) {
  return pending && uint32_t(now - last_recovery) >= kWakeRecoveryMs;
}

enum class ScanState : uint32_t { Running, PauseRequested, Paused };

constexpr bool pauseAtVsync(ScanState state, bool engine_stopped) {
  return state == ScanState::PauseRequested && engine_stopped;
}

constexpr bool resumeNeedsInit(ScanState state) {
  return state == ScanState::Paused;
}

constexpr bool pauseTimedOut(ScanState state, uint32_t elapsed_ms) {
  return state == ScanState::PauseRequested && elapsed_ms >= kScanPauseTimeoutMs;
}

constexpr bool scanPauseReady(bool pause_confirmed, bool engine_stopped,
                              uint32_t quiet_ms) {
  return pause_confirmed && engine_stopped && quiet_ms >= kScanPauseSettleMs;
}

constexpr bool cpuSleepReady(bool display_stopped, bool wifi_safe,
                             bool radio_rx_idle, bool radio_work_pending) {
  return display_stopped && wifi_safe && radio_rx_idle && !radio_work_pending;
}

constexpr bool retainedPauseUseful(bool wifi_safe, bool panel_ready,
                                    bool worker_ready, bool pause_safe) {
  return kRetainPanelForCpuSleep && wifi_safe && panel_ready && worker_ready && pause_safe;
}

#ifndef CROWPANEL_SUSPEND_RGB_WHEN_DARK
#define CROWPANEL_SUSPEND_RGB_WHEN_DARK 0
#endif
constexpr bool kSuspendRgbWhenDark = CROWPANEL_SUSPEND_RGB_WHEN_DARK != 0;

constexpr bool wakeReady(bool panel_ready, bool frame_rendered,
                         uint32_t elapsed_ms, uint32_t scanned_frames) {
  return panel_ready && frame_rendered && elapsed_ms >= kWakeSettleMs
      && scanned_frames >= kWakeScanFrames;
}

constexpr bool panelRetryDue(uint32_t now_ms, uint32_t failed_at_ms) {
  return uint32_t(now_ms - failed_at_ms) >= kPanelRetryMs;
}

constexpr uint8_t brightnessFromSlider(int value, int maximum) {
  return maximum <= 0 ? kMinBacklight
       : value <= 0 ? kMinBacklight
       : value >= maximum ? kMaxBacklight
       : kMinBacklight + (kMaxBacklight - kMinBacklight) * value / maximum;
}

constexpr uint8_t savedBrightness(uint8_t value) {
  return value >= kMinBacklight && value <= kMaxBacklight ? value : kMaxBacklight;
}

constexpr int sliderFromBrightness(uint8_t value, int maximum) {
  const int span = kMaxBacklight - kMinBacklight;
  return maximum <= 0 ? 0 :
      ((savedBrightness(value) - kMinBacklight) * maximum + span - 1) / span;
}

constexpr uint8_t nextRampLevel(uint8_t current, uint8_t target) {
  return current + kRampStep < target ? current + kRampStep : target;
}

// P4 CPLL /2 keeps MEM, SYS and APB clocks unchanged. Do not use /4 or XTAL.
constexpr uint32_t idleCpuMHz(uint32_t awake_mhz) {
  return awake_mhz == 360 || awake_mhz == 400 ? awake_mhz / 2 : awake_mhz;
}

constexpr uint32_t idlePixelClockHz(uint32_t awake_hz) {
  return awake_hz / 2;
}

constexpr uint32_t cpuClockMHz(uint32_t awake_mhz, bool screen_awake) {
  return !kDynamicScreenPower || screen_awake ? awake_mhz : idleCpuMHz(awake_mhz);
}

constexpr uint32_t pixelClockHz(uint32_t awake_hz, bool screen_awake) {
  return !kDynamicScreenPower || screen_awake ? awake_hz : idlePixelClockHz(awake_hz);
}
}  // namespace ScreenPower
