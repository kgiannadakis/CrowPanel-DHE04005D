#pragma once

#include "screen_power_policy.h"

namespace Backlight {
constexpr uint32_t kRetryMs = 1000;

constexpr uint8_t clampLevel(uint8_t level) {
  return level == 0 ? 0 : level < ScreenPower::kMinBacklight ? ScreenPower::kMinBacklight
      : level > ScreenPower::kMaxBacklight ? ScreenPower::kMaxBacklight : level;
}

// One display-task writer; producers only publish the latest requested level.
class Writes {
 public:
  constexpr int applied() const { return applied_; }
  constexpr bool offConfirmed() const { return applied_ == 0; }
  constexpr bool due(uint8_t target, bool frame_ready, uint32_t now) const {
    return target != applied_ && (target == 0 || frame_ready)
        && (!failed_ || target != attempted_ || uint32_t(now - attempted_at_) >= kRetryMs);
  }
  constexpr void record(uint8_t target, bool success, uint32_t now) {
    attempted_ = target;
    attempted_at_ = now;
    failed_ = !success;
    // A failed transaction may have partially reached the device. Do not
    // assume the old PWM is still set, especially before allowing CPU sleep.
    applied_ = success ? target : -1;
  }

 private:
  int applied_ = -1;
  int attempted_ = -1;
  uint32_t attempted_at_ = 0;
  bool failed_ = false;
};
}
