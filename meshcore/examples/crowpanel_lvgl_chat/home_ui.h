#pragma once
// ============================================================
// home_ui.h — Homescreen bubble list, enter/exit chat, delete
// ============================================================

#include <Arduino.h>
#include <lvgl.h>
#include "app_globals.h"

// Homescreen
void build_homescreen_list();
void ui_refresh_targets();

// Chat mode
void enter_chat_mode(TargetKind kind, int ch_idx, const uint8_t* pub_key);
void exit_chat_mode();

// Delete confirm
void confirm_start(ConfirmAction a, const char* msg_chat, const char* msg_serial=nullptr);
bool confirm_is_valid(ConfirmAction a);
void confirm_clear();

// Mute
void mute_set_label(const char* txt);
void ui_apply_mute_button_state();
void cb_mute_toggle(lv_event_t*);

// Back button
void cb_back_button(lv_event_t*);

// Bubble event callbacks
void cb_bubble_tapped(lv_event_t* e);
void cb_bubble_long_pressed(lv_event_t* e);
