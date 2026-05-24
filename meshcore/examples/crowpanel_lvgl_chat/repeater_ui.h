#pragma once
// ============================================================
// repeater_ui.h — Repeater screen, login, status, neighbours
// ============================================================

#include <Arduino.h>
#include <lvgl.h>
#include "app_globals.h"

// Repeater monitor
void repeater_update_monitor(const char* txt);

// Channel receipt
void clear_channel_receipt_poll();
int  count_zero_hop_repeaters();
void poll_channel_receipt_if_due();

// Repeater dropdown & buttons
void repeater_populate_dropdown();
void repeater_set_action_buttons_visible(bool visible);
void repeater_reset_state();
void repeater_reset_login();

// Callbacks
void cb_repeater_screen_loaded(lv_event_t*);
void setup_repeater_screen_callbacks();
