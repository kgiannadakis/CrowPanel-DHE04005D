#pragma once
// ============================================================
// settings_cb.h — Settings callbacks, toggles, timezone, presets
// ============================================================

#include <Arduino.h>
#include <lvgl.h>
#include "app_globals.h"

// Helpers
void reg(lv_obj_t* o, lv_event_cb_t cb, lv_event_code_t ev = LV_EVENT_CLICKED);
void btn_lbl(lv_obj_t* btn, const char* txt);

// Settings callbacks
void cb_brightness(lv_event_t*);
void cb_txpower_ready(lv_event_t*);
void cb_fadvert(lv_event_t*);
void cb_zeroadvert(lv_event_t*);
void cb_preset(lv_event_t*);

// Toggle callbacks
void cb_notifications_toggle(lv_event_t*);
void ui_apply_notifications_state();
void cb_auto_contact_toggle(lv_event_t*);
void ui_apply_auto_contact_state();
void cb_auto_repeater_toggle(lv_event_t*);
void ui_apply_auto_repeater_state();
void cb_packet_forward_toggle(lv_event_t*);
void ui_apply_packet_forward_state();
void cb_position_advert_toggle(lv_event_t*);
void ui_apply_position_advert_state();

// Speaker toggle
void ui_apply_speaker_btn_state();
void cb_speaker_toggle(lv_event_t*);

// Discover repeaters
void cb_discover_repeaters(lv_event_t*);

// Floor noise
void cb_floor_noise(lv_event_t*);

// Orientation toggle
void cb_orientation_toggle(lv_event_t*);
void ui_apply_orientation_btn_state();

// Timezone (DST-aware)
void ui_populate_timezone_dropdown();
void cb_timezone(lv_event_t*);
int32_t tz_get_effective_offset(int tz_idx, uint32_t utc_epoch);
void tz_update_offset_now();  // recalculate DISPLAY_UTC_OFFSET_S with current DST state
void tz_set_index(int idx);
int  tz_get_index();

// Presets
void ui_populate_presets();

// Keyboard focus/defocus
void settings_field_focus(lv_obj_t* field);
void settings_field_defocus(lv_obj_t* field);
void cb_textsend_focused(lv_event_t*);
void cb_textsend_defocused(lv_event_t*);
void cb_rename_focused(lv_event_t*);
void cb_rename_defocused(lv_event_t*);
void cb_timeout_focused(lv_event_t*);
void cb_timeout_defocused(lv_event_t*);
void cb_txpower_focused(lv_event_t*);
void cb_txpower_defocused(lv_event_t*);
void cb_hashtag_focused(lv_event_t*);
void cb_hashtag_defocused(lv_event_t*);
void cb_searchfield_focused(lv_event_t*);
void cb_searchfield_defocused(lv_event_t*);
void cb_searchfield_changed(lv_event_t*);
void cb_searchfield_ready(lv_event_t*);

// Ready callbacks
void cb_textsend_ready(lv_event_t*);
void cb_rename_ready(lv_event_t*);
void cb_timeout_ready(lv_event_t*);
void cb_hashtag_ready(lv_event_t*);

// Purge data
void cb_purge_data(lv_event_t*);
void purge_btn_restore_position();

// WiFi
void cb_wifi_toggle(lv_event_t*);
void cb_wifi_scan(lv_event_t*);
void cb_wifi_connect(lv_event_t*);
void cb_wifi_forget(lv_event_t*);
void cb_wifi_password_ready(lv_event_t*);
void cb_wifi_password_focused(lv_event_t*);
void cb_wifi_password_defocused(lv_event_t*);
void ui_apply_wifi_state();
void wifi_ui_update_status();

// Translation
void cb_auto_translate_toggle(lv_event_t*);
void cb_translate_lang_changed(lv_event_t*);
void ui_apply_auto_translate_state();
