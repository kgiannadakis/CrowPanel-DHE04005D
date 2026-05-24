#pragma once
//
// display.h — esp_lcd_panel_rgb + LVGL bring-up for the CrowPanel DHE04005D
// (ESP32-P4 + 5" 800x480 ST7262 RGB panel) — selector firmware variant.
//
// Slimmed-down fork of meshcore/examples/crowpanel_p4_lvgl_chat/display.{h,cpp}:
// landscape-only, no chat gesture handling, no portrait/PPA path. Touch read
// just emits LVGL press/release at the (-8, -1) compensated point.
//
#include <lvgl.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool display_init(void);
void display_start_lvgl_task(void);
bool display_touch_attach(void);

bool lvgl_lock(int timeout_ms);   // timeout_ms < 0 = wait forever
void lvgl_unlock(void);

#ifdef __cplusplus
}
#endif
