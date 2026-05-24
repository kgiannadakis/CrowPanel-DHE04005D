#pragma once
#if HAS_TFT && USE_MCUI

#include <stdint.h>

namespace mcui {

void mcclock_init();

void mcclock_save(uint32_t utc_epoch);

bool mcclock_has_rtc();

bool mcclock_has_valid_time();

}

#endif
