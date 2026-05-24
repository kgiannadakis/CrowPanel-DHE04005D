#pragma once
// ============================================================
// chat_ui.h — Chat screen rendering (bubbles, header, scroll)
// ============================================================

#include <Arduino.h>
#include <lvgl.h>
#include "app_globals.h"

// Chat bubble rendering
lv_obj_t* chat_add(bool out, const char* txt, bool live = false, char loaded_status = 0, const char* signal_info = nullptr, uint16_t loaded_repeat_count = 0, lv_obj_t** bubble_out = nullptr, const char* translation = nullptr);
void apply_status_to_label(lv_obj_t* lbl, char sc);
void chat_add_resend_btn(lv_obj_t* status_label, const uint8_t* pub_key, const char* text);
void apply_receipt_count_to_label(lv_obj_t* lbl, uint16_t count, int8_t snr_raw);

// Chat scroll
void chat_scroll_to_newest();
// Apply keyboard-aware bottom padding to ui_chatpanel without
// scrolling. Use this before lv_obj_scroll_to_y() when targeting
// a specific position (e.g. the NEW-MESSAGES divider) so the
// scrollable area has reserved space for the text input / keyboard
// before the scroll math runs.
void chat_apply_kb_aware_bottom_pad();
void scroll_btn_ensure();
void cb_chatpanel_scroll(lv_event_t*);
void cb_scroll_to_newest(lv_event_t*);

// Chat header
void chat_update_route_label();
void chat_set_header(const char* name);

// Chat clear
void chat_clear();
