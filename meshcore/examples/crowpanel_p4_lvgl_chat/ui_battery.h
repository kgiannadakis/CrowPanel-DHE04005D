#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// All calls run under the LVGL lock. New headers inherit the last UI reading.
void ui_battery_create(lv_obj_t* header);
void ui_battery_update(int percent, uint32_t color);

#ifdef __cplusplus
}
#endif
