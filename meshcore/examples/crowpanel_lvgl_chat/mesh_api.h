#pragma once
// ============================================================
// mesh_api.h — Thin wrappers for UIMesh-specific methods
// Allows extracted modules to call UIMesh without full type.
// Implemented in main.cpp.
// ============================================================

#include <Arduino.h>

struct Preset;  // defined in app_globals.h

void mesh_set_tx_power_pref(int8_t dbm);
void mesh_send_zero_hop_advert(int delay_ms);
uint32_t mesh_send_discover_repeaters();
void mesh_rename_if_non_empty(const char* name);
int  mesh_join_hashtag_channel(const String& raw);
void mesh_purge_contacts_and_repeaters();
int  mesh_current_channel_idx();
bool mesh_select_contact_by_pubkey(const uint8_t* pub_key);
bool mesh_select_channel_by_idx(int idx);
bool mesh_delete_contact_by_pubkey(const uint8_t* pub_key);
bool mesh_delete_repeater_by_pubkey(const uint8_t* pub_key);
bool mesh_delete_channel_by_idx(int idx);
bool mesh_delete_selected_channel();
void mesh_stop_repeater_connection(const uint8_t* pub_key);
const Preset* mesh_presets(int& count);
void mesh_apply_preset_by_idx(int idx, bool do_advert = true);
void mesh_send_flood_advert(int delay_ms = 0);
void mesh_reset_current_contact_path();
bool mesh_discover_contact_path();

// Direct send without changing UI selection (for bridge/dashboard)
bool mesh_send_text_to_contact(const uint8_t* pub_key, const char* text);
bool mesh_send_text_to_channel(int idx, const char* text);

// Resend the PM whose status_label is `status_label`. Reuses the same ring
// slot / bubble / file entry. Returns true if a matching slot was found and
// retransmission was queued.
struct _lv_obj_t;
bool mesh_resend_pm_by_bubble_label(struct _lv_obj_t* status_label);
const char* mesh_get_node_name();
void mesh_get_fixed_position(double* lat, double* lon);
void mesh_set_fixed_position(double lat, double lon);
void mesh_clear_fixed_position();

void mesh_populate_repeater_list();

// Repeater operations (for web dashboard)
int  mesh_repeater_login(const uint8_t* pub_key, const char* password);
int  mesh_repeater_request_status(const uint8_t* pub_key);
int  mesh_repeater_request_neighbours(const uint8_t* pub_key);
int  mesh_repeater_send_advert(const uint8_t* pub_key);
int  mesh_repeater_send_reboot(const uint8_t* pub_key);
