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

// ---- Selected-repeater accessors -----------------------------------
// All readers should go through these instead of dereferencing the
// short-lived g_selected_repeater pointer. The saved pubkey + name
// snapshot is captured at dropdown-select time and survives any
// contacts[] shuffles.

// Copy the saved selected-repeater pubkey into out[32]. Returns false
// if no repeater has been selected (or selection was cleared).
bool repeater_get_selected_pubkey(uint8_t out[32]);

// Returns the saved name of the currently-selected repeater, or "" if
// nothing is selected. Pointer is valid for the lifetime of the
// process (points into a static buffer).
const char* repeater_get_selected_name();

// Set the selected-repeater snapshot from a known contact (used by
// non-UI paths like the web dashboard so retry logic shares the same
// stable pubkey/name source as the repeater screen).
void repeater_set_selected_snapshot(const uint8_t pubkey[32], const char* name);

// ---- CLI terminal hooks --------------------------------------------
// True when the user has tapped the CLI button and the monitor area
// is acting as a terminal.
bool repeater_cli_mode_active();

// In CLI mode the on-screen keyboard is shown programmatically and the
// input textarea never receives LVGL focus, so the DEFOCUSED-based hide
// never fires on a background tap. The touch read callback calls this on
// every fresh press: if CLI mode is active and the press lands outside
// both the CLI keyboard and the CLI input, it requests a deferred
// keyboard dismiss (handled in loop() via g_dismiss_keyboard).
void repeater_cli_dismiss_kb_if_outside(int x, int y);

// Hide the CLI on-screen keyboard (without leaving CLI mode). Called from
// loop() when a tap-outside dismiss was requested.
void repeater_cli_hide_keyboard();

// Forward an incoming TXT_TYPE_CLI_DATA reply into the terminal. No-op
// unless CLI mode is active AND the sender pubkey matches the
// currently-selected repeater. Called from UIMesh::onCommandDataRecv.
void repeater_cli_handle_response(const uint8_t* sender_pubkey, const char* text);
