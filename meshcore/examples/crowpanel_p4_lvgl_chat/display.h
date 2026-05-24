#pragma once
//
// display.h — direct esp_lcd_panel_rgb + LVGL bring-up for CrowPanel DHE04005D.
//
// Deliberately bypasses the ESP32_Display_Panel Arduino library, whose v1.0.4
// C++ static-initialisers collide with pioarduino's newer ESP-IDF I2C driver.
//
#include <lvgl.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bring up the 800x480 RGB panel + LVGL core. DOES NOT start the LVGL service
// task — after this returns, main.cpp can safely create widgets from its own
// thread because nothing else is touching LVGL yet. Returns true on success.
bool display_init(void);

// Start the LVGL service task. Call this AFTER all setup()-time widget
// creation is done; after this call, any widget touch from outside the task
// MUST be bracketed with lvgl_lock()/lvgl_unlock().
void display_start_lvgl_task(void);

// Wire the GT911 touch controller (gt911.cpp) into LVGL as an input device.
// Must be called AFTER display_init() and AFTER stc8 is up (GT911 reset goes
// through the STC8 coprocessor). Returns true if touch is live.
bool display_touch_attach(void);

// LVGL is single-threaded. Any code that touches LVGL widgets from a task
// other than the LVGL task itself MUST bracket the access in lvgl_lock()
// / lvgl_unlock().
bool lvgl_lock(int timeout_ms);   // timeout_ms < 0 = wait forever
void lvgl_unlock(void);

// If a PPA timeout-and-reset happened inside flush_cb, the framebuffer
// is stuck on whatever was last successfully written. This function
// invalidates the active screen so the next frame pulls fresh content
// out of LVGL. Caller must hold lvgl_lock(). Idempotent — no-op when
// nothing's pending.
void display_handle_ppa_recovery_if_pending(void);

#ifdef __cplusplus
}
#endif

// --- v11 compatibility helpers (defined in keyboard_helpers.cpp / utils.cpp) ---
#include <lvgl.h>
// Populate SCR_W/SCR_H/STATUS_H/… from the current g_landscape_mode value.
// MUST run before ui_init() — the SquareLine screens read these globals at
// widget-create time, so flipping orientation after ui_init() has no effect
// until the screens are rebuilt.
void init_layout_constants();
void kb_apply_language(lv_obj_t* kb);
void setup_keyboard(lv_obj_t* kb);
void kb_hide(lv_obj_t* kb, lv_obj_t* ta);
void kb_show(lv_obj_t* kb, lv_obj_t* ta);

void speaker_init();
void beep_msg_in();
void beep_msg_out();
void beep_error();
// Short test tone fired when the user releases the volume slider —
// lets them hear the new level without waiting for a real event.
void speaker_play_preview();

// Notification-sound bank. Keep the dropdown + beep dispatcher in
// sync via these helpers. notification_sound_test() previews an
// arbitrary index without committing it as g_notification_sound_idx.
int         notification_sound_count();
const char* notification_sound_name(int idx);
const char* notification_sound_names_newline_list();  // for lv_dropdown
void        notification_sound_test(int idx);
