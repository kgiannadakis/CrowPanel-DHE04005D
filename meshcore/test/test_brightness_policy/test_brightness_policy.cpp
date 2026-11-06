#include "../../examples/crowpanel_p4_lvgl_chat/screen_power_policy.h"

static_assert(ScreenPower::savedBrightness(0) == 100);
static_assert(ScreenPower::savedBrightness(4) == 100);
static_assert(ScreenPower::savedBrightness(5) == 5);
static_assert(ScreenPower::savedBrightness(100) == 100);
static_assert(ScreenPower::savedBrightness(101) == 100);
static_assert(ScreenPower::savedBrightness(255) == 100);
static_assert(ScreenPower::sliderFromBrightness(5, 100) == 0);
static_assert(ScreenPower::sliderFromBrightness(100, 100) == 100);
static_assert(ScreenPower::sliderFromBrightness(50, 0) == 0);

constexpr bool all_levels_round_trip() {
  for (int level = 5; level <= 100; ++level) {
    const int slider = ScreenPower::sliderFromBrightness(level, 100);
    if (slider < 0 || slider > 100
        || ScreenPower::brightnessFromSlider(slider, 100) != level) return false;
  }
  for (int slider = 0; slider <= 100; ++slider) {
    const uint8_t level = ScreenPower::brightnessFromSlider(slider, 100);
    if (ScreenPower::savedBrightness(level) != level
        || ScreenPower::brightnessFromSlider(
            ScreenPower::sliderFromBrightness(level, 100), 100) != level) return false;
  }
  return true;
}
static_assert(all_levels_round_trip());
