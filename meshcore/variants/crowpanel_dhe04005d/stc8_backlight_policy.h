#pragma once

#include <stdint.h>

namespace Stc8Backlight {
constexpr uint8_t kStartDuty = 100;
constexpr uint32_t kStartMs = 30;
constexpr bool needsStart(int previous, uint8_t duty) {
  return previous <= 0 && duty > 0 && duty < kStartDuty;
}
}
