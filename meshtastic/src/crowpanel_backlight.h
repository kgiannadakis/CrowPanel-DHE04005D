#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void backlight_notify_activity(void);

bool backlight_is_screen_on(void);

void backlight_set_timeout_secs(uint32_t secs);

void backlight_i2c_lock(void);
void backlight_i2c_unlock(void);

// Boot framebuffer gate. esp_lcd_new_rgb_panel() permanently corrupts the
// PSRAM TLSF heap on this P4 board, so the mcui UI task blocks on
// crowpanel_p4_fb_gate_wait() right before that call until main.cpp's
// early-network init has brought WiFi up and opened the MQTT socket
// (crowpanel_p4_fb_gate_release()) — putting those lwIP/ESP-Hosted PSRAM
// allocations on the still-clean heap. The wait has a timeout backstop so a
// stalled main thread can never leave the display permanently dark.
void crowpanel_p4_fb_gate_wait(uint32_t timeout_ms);
void crowpanel_p4_fb_gate_release(void);

#ifdef __cplusplus
}
#endif
