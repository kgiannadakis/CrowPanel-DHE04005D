#include "../../examples/crowpanel_p4_lvgl_chat/screen_power_policy.h"

using namespace ScreenPower;

static_assert(brightnessFromSlider(0, 100) == 5, "Minimum must be 5% duty");
static_assert(brightnessFromSlider(50, 100) == 52, "Slider must cover the full range");
static_assert(brightnessFromSlider(100, 100) == 100, "Maximum must remain 100%");
static_assert(brightnessFromSlider(-1, 100) == 5, "Clamp below the slider range");
static_assert(brightnessFromSlider(101, 100) == 100, "Clamp above the slider range");
static_assert(brightnessFromSlider(0, 0) == 5, "Handle invalid slider maximum");
static_assert(brightnessFromSlider(1, 10) == 14, "Support other slider ranges");
static_assert(kBacklightOff == 0 && kMinBacklight > kBacklightOff, "Off must remain distinct");
static_assert(idleCpuMHz(360) == 180, "ES P4 must retain its bus clocks");
static_assert(idleCpuMHz(400) == 200, "Production P4 must retain its bus clocks");
static_assert(idleCpuMHz(180) == 180, "Do not further divide an already reduced CPU");
static_assert(idleCpuMHz(160) == 160, "Leave unfamiliar configurations unchanged");
static_assert(idlePixelClockHz(18000000) == 9000000, "Halve dark-screen scanout");
static_assert(cpuClockMHz(360, true) == 360, "Awake CPU frequency stays unchanged");
static_assert(cpuClockMHz(360, false) == (kDynamicScreenPower ? 180 : 360),
              "Stable mode must not change CPU frequency during backlight-off");
static_assert(pixelClockHz(18000000, true) == 18000000, "Awake scanout stays unchanged");
static_assert(pixelClockHz(18000000, false) == (kDynamicScreenPower ? 9000000 : 18000000),
              "Stable mode must retain full scanout speed while dark");
static_assert(kCpuLightSleep == (CROWPANEL_CPU_LIGHT_SLEEP != 0),
              "CPU sleep must be independent of dynamic screen clocks");
static_assert(kRetainPanelForCpuSleep == (kCpuLightSleep && !kDynamicScreenPower),
              "Normal clocks must use retained-panel pause rather than teardown");
static_assert(!pauseAtVsync(ScanState::Running, true), "Normal VSYNC must not pause scanout");
static_assert(!pauseAtVsync(ScanState::PauseRequested, false),
              "A pending request must never truncate a running frame");
static_assert(pauseAtVsync(ScanState::PauseRequested, true),
              "Only a completed final frame confirms the pause");
static_assert(!pauseAtVsync(ScanState::Paused, true), "A late VSYNC must not repeat the pause");
static_assert(!resumeNeedsInit(ScanState::Running), "A running panel must not be reinitialized");
static_assert(!resumeNeedsInit(ScanState::PauseRequested),
              "Rapid wake must wait for the final frame, not reset active DMA");
static_assert(resumeNeedsInit(ScanState::Paused), "A confirmed pause needs a serialized restart");
static_assert(!pauseTimedOut(ScanState::PauseRequested, 249), "Allow a frame to finish normally");
static_assert(pauseTimedOut(ScanState::PauseRequested, 250), "Bound an unconfirmed pause");
static_assert(!pauseTimedOut(ScanState::Running, 250), "Do not recover a normal running panel");
static_assert(!pauseTimedOut(ScanState::Paused, 250), "A confirmed pause is not a timeout");
static_assert(pauseTimedOut(ScanState::PauseRequested, uint32_t(20 - (UINT32_MAX - 229))),
              "The pause timeout must handle millis wrap");
static_assert(!scanPauseReady(false, true, 60), "A pause request alone must not allow CPU sleep");
static_assert(!scanPauseReady(true, false, 60), "Running LCD engine blocks CPU sleep");
static_assert(!scanPauseReady(true, true, 59), "Allow pending scan interrupts to settle");
static_assert(scanPauseReady(true, true, 60), "A stopped, settled engine allows sleep");
static_assert(scanPauseReady(true, true, uint32_t(20 - (UINT32_MAX - 39))),
              "Pause settling must handle millis wrap");
static_assert(!wakeReady(false, true, 120, 2), "No light without a recreated panel");
static_assert(!wakeReady(true, false, 120, 2), "No light without a completed redraw");
static_assert(!wakeReady(true, true, 119, 2), "Allow the panel to settle");
static_assert(!wakeReady(true, true, 120, 1), "Wait for two frames after redraw");
static_assert(wakeReady(true, true, 120, 2), "Allow light only after all wake checks");
static_assert(!panelRetryDue(1999, 1000), "Do not retry allocation continuously");
static_assert(panelRetryDue(2000, 1000), "Retry after one second");
static_assert(!panelRetryDue(499, UINT32_MAX - 499), "Retry delay handles millis wrap");
static_assert(panelRetryDue(500, UINT32_MAX - 499), "Retry becomes due across millis wrap");
static_assert(nextRampLevel(5, 5) == 5, "Wake at minimum brightness must complete");
static_assert(nextRampLevel(5, 100) == 15, "Keep wake ramp duration bounded");
static_assert(nextRampLevel(95, 100) == 100, "Do not overshoot the target");
static_assert(nextRampLevel(15, 10) == 10, "Clamp if target decreases during ramp");
static_assert(kCpuSleepMs > 0 && kCpuSleepMs <= kDarkUiPeriodMs,
              "Timer wake must keep dark-screen application servicing bounded");
static_assert(kCpuSleepMs == CROWPANEL_CPU_SLEEP_MS, "Allow the original 20 ms cadence for A/B tests");
static_assert(kDarkUiPeriodMs <= 80, "Keep polling-only touch wake responsive");
static_assert(tickPeriodMs(true, 2) == 2, "Normal awake LVGL timing must be unchanged");
static_assert(tickPeriodMs(false, 2) == (kBacklightOnlySleep ? 2 : 80),
              "Backlight-only sleep keeps the normal tick; experiments can coalesce it");
static_assert(cpuSleepPeriodMs(false) == kCpuSleepMs,
              "Unverified touch must keep the established polling fallback");
static_assert(cpuSleepPeriodMs(true) == (kCpuLightSleep && kTouchIrqSleep ? kTouchIrqSleepMs : kCpuSleepMs),
              "Only verified and enabled GPIO wake permits longer CPU sleeps");
static_assert(kTouchIrqSleepMs <= 500, "Background work must retain a bounded timer fallback");
static_assert(wakeInputsQuiet(false, true, false, false), "Idle GPIOs allow extended sleep");
static_assert(!wakeInputsQuiet(true, true, false, false), "An arriving radio IRQ prevents sleep");
static_assert(!wakeInputsQuiet(false, true, true, false), "An arriving touch prevents sleep");
static_assert(!wakeInputsQuiet(false, false, false, true), "A waking screen prevents sleep");
static_assert(wakeInputsQuiet(false, false, true, false), "Polling fallback has no touch GPIO gate");
static_assert(uint32_t(20 - (UINT32_MAX - 59)) == 80,
              "Elapsed tick updates must work across the millisecond counter wrap");

constexpr bool sleepGatesAreStrict() {
  for (unsigned mask = 0; mask < 16; ++mask) {
    const bool stopped = mask & 1;
    const bool wifi_safe = mask & 2;
    const bool rx_idle = mask & 4;
    const bool tx_pending = mask & 8;
    if (cpuSleepReady(stopped, wifi_safe, rx_idle, tx_pending) != (mask == 7))
      return false;
  }
  return true;
}
static_assert(sleepGatesAreStrict(), "Sleep requires every gate; pending TX always blocks it");

constexpr bool rampCompletes() {
  uint8_t level = kMinBacklight;
  for (int step = 0; step < 10; ++step) level = nextRampLevel(level, kMaxBacklight);
  return level == kMaxBacklight;
}
static_assert(rampCompletes(), "Wake ramp must reach maximum in at most ten steps");

int main() { return 0; }
