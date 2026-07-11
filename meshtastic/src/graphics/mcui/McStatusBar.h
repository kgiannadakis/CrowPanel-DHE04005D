#pragma once
#if HAS_TFT && USE_MCUI
#include <lvgl.h>
namespace mcui {

lv_obj_t *statusbar_create(lv_obj_t *parent);

// Poll hardware sensors (battery over I2C). Does NOT touch LVGL, so it must be
// called WITHOUT the LVGL lock held — keeps the blocking I2C read off the
// render/touch path. Self-throttled internally.
void statusbar_poll_battery();

// Apply cached state to the LVGL widgets. Must be called WITH the LVGL lock.
void statusbar_refresh();

void statusbar_set_visible(bool visible);
}
#endif
