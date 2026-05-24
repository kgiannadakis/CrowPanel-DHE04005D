#pragma once

#include <lvgl.h>

namespace crowpanel_p4 {

bool display_init();
void display_start_lvgl_task();

lv_display_t* lv_display();
lv_indev_t*   lv_indev();

bool lvgl_lock(int timeout_ms);
void lvgl_unlock();

}
