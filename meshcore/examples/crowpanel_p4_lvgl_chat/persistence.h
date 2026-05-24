#pragma once
// ============================================================
// persistence.h — LittleFS chat files, NVS prefs, contact records
// ============================================================

#include <Arduino.h>
#include "app_globals.h"

// On-disk contact record
#pragma pack(push, 1)
struct ContactRecord {
  uint8_t  pub_key[32];
  char     name[32];
  uint8_t  type;
  uint8_t  flags;
  uint8_t  out_path_len;
  uint32_t last_advert_timestamp;
  uint8_t  out_path[64];
  int32_t  gps_lat;
  int32_t  gps_lon;
};
#pragma pack(pop)

// "NEW" divider tracking
struct ChatReadPos {
  char key[20];
  int  msg_count;
  bool valid;
};
static const int MAX_READ_POS = 64;
extern ChatReadPos g_read_pos[MAX_READ_POS];

int read_pos_find(const char* key);
int read_pos_get(const char* key);
void read_pos_set(const char* key, int count);

// Chat key helpers
String key_for_contact(const mesh::Identity& id);
String key_for_channel(int idx);
String chat_path_for(const String& key);

// Chat file I/O
void append_chat_to_file(const String& key, bool out, const char* msg, uint32_t msg_ts = 0, const char* signal_info = nullptr);
void update_last_tx_status_in_file(const String& key, char status, int16_t repeat_count = -1);
// Append a translation to the newest RX line in `key`'s chat file whose
// body equals `match_text` and that doesn't already carry a {{TR}}
// marker. Matching by body avoids the earlier bug where a translation
// would land on whichever RX line happened to be "last untranslated"
// at write time — a problem as soon as >1 message is queued.
void append_translation_to_last_rx(const String& key, const char* match_text, const char* translation);
void update_tx_status_by_msg_ts(const String& key, uint32_t msg_ts, char status);
void load_chat_from_file(const String& key);
void delete_chat_file_for_key(const String& key);

// Coalesced chat-write flush controls. append_chat_to_file() buffers
// lines and marks a 200 ms deadline in g_chat_flush_deadline_ms; the
// main loop drains via chat_append_flush_if_due() once the deadline
// elapses. Call chat_append_flush_sync() before any read that needs
// to see the very latest messages (chat load, chat switch).
extern uint32_t g_chat_flush_deadline_ms;
void chat_append_flush_if_due();
void chat_append_flush_sync();

// NVS preferences
uint32_t clamp_timeout_s(uint32_t s);
// Request a persist of the UI prefs block. Marks a dirty flag and a
// debounce timestamp — the actual NVS write happens from the main loop
// once the flip has been quiet for ~500 ms. This prevents every toggle
// tap from costing a synchronous write of 13 NVS keys to flash.
void save_ui_prefs_nvs();
// Synchronous write. Called by the main-loop debouncer and from any
// path that genuinely needs to ensure the block hits flash right now
// (e.g., before esp_restart() on orientation change).
void save_ui_prefs_nvs_now();
void load_ui_prefs_nvs();
void save_timeout_s(uint32_t s);
void save_device_name_nvs(const char* name);
bool load_device_name_nvs(char* out, size_t outlen);
void save_preset_idx_nvs(int idx);
int load_preset_idx_nvs();
void save_notifications_nvs();
void load_notifications_nvs();
void save_landscape_nvs(bool landscape);
bool load_landscape_nvs();

// Contact file persistence
void rebuild_contacts_file_excluding(const uint8_t* pub32);
void purge_contacts_file_all();

// Resolve pending TX on boot
void resolve_pending_in_file(const String& path);
void resolve_pending_on_boot();

// Channels file path
extern const char* CHANNELS_FILE;
