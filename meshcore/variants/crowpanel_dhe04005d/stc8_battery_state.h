#pragma once

#include <stdint.h>

// Elecrow EM_BAT_CHARGE_STATE, battery register offset 9.
enum {
  STC8_BAT_IDLE          = 0,
  STC8_BAT_CHARGING      = 1,
  STC8_BAT_FULLY_CHARGED = 2,
  STC8_BAT_DISCHARGING   = 3,  // BAT_CHARGE_NO_CHARGE
  STC8_BAT_ERROR         = 4,
};

#ifdef __cplusplus
namespace Stc8Battery {
constexpr bool isCharging(uint8_t state) {
  return state == STC8_BAT_CHARGING || state == STC8_BAT_FULLY_CHARGED;
}
}
#endif
