#pragma once
// ============================================================
// display.h — LVGL display driver, touch, keyboard maps
// ============================================================

#include <Arduino.h>
#include <lvgl.h>
#include "app_globals.h"

// Display init
void init_display_and_ui();

// Speaker
void speaker_init();
void beep_short(uint16_t freq_hz, uint16_t duration_ms);
void beep_msg_in();
void beep_msg_out();
void beep_error();

// Keyboard
void kb_apply_language(lv_obj_t* kb);
void setup_keyboard(lv_obj_t* kb);
void kb_hide(lv_obj_t* kb, lv_obj_t* ta);
void kb_show(lv_obj_t* kb, lv_obj_t* ta);
