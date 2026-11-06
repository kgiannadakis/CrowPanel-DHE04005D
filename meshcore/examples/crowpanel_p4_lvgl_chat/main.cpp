// ============================================================
// main.cpp — UIMesh class, setup(), loop(), global definitions
// ============================================================

#include <Arduino.h>
#include <atomic>
#include <Wire.h>
#include <lvgl.h>
#include <extra/widgets/keyboard/lv_keyboard.h>
#include <esp_task_wdt.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_sleep.h>
#include <esp_timer.h>
#include <driver/gpio.h>
#include "crash_diagnostics.h"
#include <WiFi.h>
// P4 port: v11's LovyanGFX driver is replaced by our direct esp_lcd_panel_rgb
// bring-up in display.cpp. Also pull in the STC8 helper (backlight PWM) and
// the SDIO pin macros (for WiFi via the on-board C6).
#include "stc8.h"
#include "board_config.h"
#include "gt911.h"

extern "C" void lv_font_greek_init(void);   // v11 Greek-overlay font shim init

// SquareLine
#include "ui.h"
#include "ui_homescreen.h"
#include "ui_settingscreen.h"
#include "ui_repeaterscreen.h"

extern "C" void _ui_screen_change(lv_obj_t** target, lv_scr_load_anim_t fademode, int spd, int delay, void (*target_init)(void));

#include <Mesh.h>

#if defined(NRF52_PLATFORM)
  #include <InternalFileSystem.h>
#elif defined(RP2040_PLATFORM)
  #include <LittleFS.h>
#elif defined(ESP32)
  // P4 port: swapped SPIFFS for LittleFS (SPIFFS's 10 MB format hangs on P4,
  // see variants/crowpanel_dhe04005d/target.cpp). Explicit source edits
  // below replace every SPIFFS call with LittleFS — a macro #define collides
  // with the SPIFFS library's own extern declarations.
  #include <LittleFS.h>
  #include <Preferences.h>
#endif

#include <helpers/ArduinoHelpers.h>
#include <helpers/StaticPoolPacketManager.h>
#include <helpers/SimpleMeshTables.h>
#include <helpers/IdentityStore.h>
#include <helpers/BaseChatMesh.h>
#include <helpers/TransportKeyStore.h>

#include <target.h>
#include <SHA256.h>

// Split module headers
#include "app_globals.h"
#include "utils.h"
#include "persistence.h"
#include "chat_ui.h"
#include "home_ui.h"
#include "display.h"
#include "settings_cb.h"
#include "repeater_ui.h"
#include "map_view.h"
#include "neighbour_contact_policy.h"
#include "mesh_api.h"
#include "wifi_ntp.h"
#include "features_ui.h"
#include "features_cb.h"
#include "ota_update.h"
#include "web_dashboard.h"
#include "telegram_bridge.h"
#include "translate.h"
#include "sd_storage.h"
#include "emoji_atlas.h"
#include "emoji_font.h"

// ============================================================
// Global variable DEFINITIONS
// (declared extern in app_globals.h, defined here)
// ============================================================

// P4 port: no LovyanGFX on P4 — display is driven by our display.cpp.

// ---- Display / LVGL ----
lv_obj_t *serialLabel            = nullptr;
lv_obj_t *g_pending_status_label = nullptr;  // channel receipts only
lv_obj_t *g_scroll_btn           = nullptr;
lv_obj_t *g_chat_header_label    = nullptr;
lv_obj_t *g_chat_route_label     = nullptr;
char      g_chat_target_name[64] = "";
bool      g_in_chat_mode         = false;
char      g_current_chat_key[20] = "";
char      g_search_filter[64]    = "";
lv_coord_t g_chatpanel_orig_y    = 0;
bool      g_loading_history      = false;
bool      g_notifications_dirty  = false;

// ---- Delete-pending state ----
DeletePending g_del                  = {};
bool          g_long_press_just_fired = false;

// ---- Deferred state ----
char   g_deferred_send_text[241] = "";
bool   g_deferred_send_pending   = false;
bool   g_deferred_scroll_bottom  = false;

// Deferred radio actions
bool   g_deferred_flood_advert   = false;
bool   g_deferred_zero_advert    = false;
int    g_deferred_preset_idx     = -1;

// Contact save debounce
bool     g_contacts_save_dirty   = false;
uint32_t g_contacts_save_ms      = 0;
bool     g_ui_prefs_save_dirty   = false;
uint32_t g_ui_prefs_save_ms      = 0;
bool     g_suppress_next_scroll_bottom = false;
// Divider widget (the "NEW MESSAGES" row inserted by load_chat_from_file)
// that the main loop should scroll into view once LVGL has done a full
// layout pass. Deferring avoids the "panel hasn't laid out yet → divider_y
// returns 0 → scrolls to top" race that used to happen when scrolling
// was done inline at the end of load_chat_from_file.
lv_obj_t* g_pending_scroll_to_divider = nullptr;
bool     g_map_viewport_save_dirty = false;
uint32_t g_map_viewport_save_ms    = 0;
// Tracks whether ui_chatpanel's children are the output of the last
// build_homescreen_list() call. Flipped off by chat_add() / chat_clear()
// when they mutate the panel with non-home content. build_homescreen_list()
// uses this to decide whether its hash-based skip is valid — without the
// flag, a chat_exit would find the hash unchanged and skip rebuilding,
// leaving chat bubbles on-screen after the user swiped back.
bool     g_homescreen_list_is_current = false;

// Deferred ACK/sound
char   g_deferred_status_char    = 0;
uint8_t g_deferred_sound = 0;

// Deferred LVGL operations
bool   g_deferred_refresh_targets = false;
bool   g_deferred_serialmon_dirty = false;
bool   g_deferred_timelabel_dirty = false;
bool   g_deferred_battery_dirty = false;
bool   g_deferred_route_label_dirty = false;
bool   g_deferred_wifi_status_dirty = false;
bool   g_deferred_receipt_update = false;

// Deferred repeater monitor
char   g_deferred_repeater_mon[640] = "";
bool   g_deferred_repeater_mon_dirty = false;
int8_t g_deferred_repeater_btns = 0;

// Deferred chat messages
DeferredChatMsg g_deferred_msgs[DEFERRED_MSG_MAX];
int g_deferred_msg_count = 0;
int g_deferred_msg_dropped = 0;
bool g_deferred_swipe_back = false;
bool g_deferred_swipe_home = false;

// ---- Features ----
bool     g_tgbridge_enabled       = false;
bool     g_webdash_enabled        = false;
bool     g_deferred_features_dirty = false;

// ---- Screen state ----
volatile bool     g_screen_awake   = true;
volatile uint32_t g_last_touch_ms  = 0;
uint8_t           g_backlight_level = BL_MAX;
bool              g_swallow_touch   = false;
bool              g_touch_was_press = false;
bool              g_dismiss_keyboard = false;
uint32_t          g_screen_timeout_s = 30;

// ---- Feature flags ----
bool g_notifications_enabled = true;
bool g_auto_contact_enabled  = true;
bool g_auto_repeater_enabled = true;
bool g_packet_forward_enabled = true;
bool g_position_advert_enabled = true;
bool g_fixed_position_valid = false;
bool g_fixed_position_apply_pending = false;
double g_fixed_position_lat = 0.0;
double g_fixed_position_lon = 0.0;
bool g_auto_translate_enabled = false;
int  g_translate_lang_idx     = 0;
uint8_t g_text_header_bytes   = 1;
bool g_manual_discover_active = false;
uint32_t g_discover_tag = 0;
bool g_deferred_discover_done = false;
static DiscoveryResults s_discovery_results;
#if defined(ESP32)
static portMUX_TYPE s_discovery_mux = portMUX_INITIALIZER_UNLOCKED;
#endif
bool g_purged_this_session   = false;
bool    g_speaker_enabled = true;
uint8_t g_speaker_volume  = 60;  // 0..100; persisted in NVS "ui/spk_vol"
uint8_t g_notification_sound_idx = 0;  // persisted in NVS "ui/snd_idx"

// ---- Notification tracking ----
ContactUnread g_contact_unread[MAX_UNREAD_SLOTS] = {};
ChannelUnread g_channel_unread[MAX_UNREAD_SLOTS] = {};

// ---- SNR tracking ----
ContactSNR g_contact_snr[MAX_UNREAD_SLOTS] = {};

// ---- Chat route indicator ----
uint8_t  g_chat_route_path_len = OUT_PATH_UNKNOWN;
bool     g_chat_route_is_contact = false;

// ---- Time ----
// No hardware RTC on this variant. NTP is the sole clock source.
int32_t DISPLAY_UTC_OFFSET_S = 3600;

// ---- Ramp state ----
uint8_t  g_ramp_target   = BL_MAX;
uint8_t  g_ramp_current  = 0;
uint32_t g_ramp_next_ms  = 0;

// ---- Swipe tracking ----
uint16_t g_swipe_start_x = 0;
uint16_t g_swipe_start_y = 0;
bool     g_swipe_tracking = false;

// ---- Dropdown data ----
DropdownEntry dd_channels[MAX_DD_ENTRIES];
int           dd_channels_count = 0;
DropdownEntry dd_contacts[MAX_DD_ENTRIES];
int           dd_contacts_count = 0;
char dd_opts_buf[MAX_DD_ENTRIES * 41];

// ---- Channel mask ----
uint32_t g_deleted_channel_mask = 0;
uint32_t g_muted_channel_mask   = 0;

// ---- Homescreen bubble pool ----
BubbleTapData g_bubble_pool[MAX_DD_ENTRIES * 2];
int           g_bubble_pool_count = 0;

// ---- Channel receipt poll ----
ChannelReceiptPoll g_channel_receipt = {};

// ---- Repeater ----
bool         g_repeater_logged_in     = false;
uint8_t      g_login_pending_key[4]   = {};
uint32_t     g_login_timeout_ms       = 0;
uint8_t      g_login_retry_count      = 0;
char         g_login_last_pw[16]      = {};
uint32_t     g_status_pending_key     = 0;
uint32_t     g_neighbours_pending_key = 0;
ContactInfo* g_selected_repeater  = nullptr;
RepeaterListEntry g_repeater_list[MAX_DD_ENTRIES] = {};
int          g_repeater_count = 0;

// ---- Serial monitor buffers ----
// Trimmed from 2 KB to 1 KB — the serial monitor label renders this
// entire buffer on every refresh (label does a full relayout for any
// text change), so cutting the size proportionally cuts the
// per-refresh CPU cost and returns always-on internal RAM.
const size_t SERIAL_BUF_SIZE = 1024;
const size_t SERIAL_BUF_TRIM = SERIAL_BUF_SIZE / 2;
char  g_serial_buf[1024 + 1];
char  g_serial_buf_front[1024 + 1];
size_t g_serial_len = 0;

// ---- Speaker / Discover buttons ----
lv_obj_t* g_speaker_btn = nullptr;
lv_obj_t* g_discover_repeaters_btn = nullptr;
lv_obj_t* g_pkt_fwd_btn = nullptr;

// ---- Keyboard state ----
bool g_kb_greek = false;
uint8_t g_kb_lang = 0;  // 0=EN, 1=EL, 2=DE

// ---- Confirm action ----
ConfirmAction g_confirm_action = ConfirmAction::NONE;
uint32_t g_confirm_deadline_ms = 0;

// ---- Repeater screen ----
bool g_repeater_cbs_wired = false;
char g_repeater_filter[32] = "";

// ---- Per-message PM tracking ring buffer ----
OutboundPM g_pm_ring[PM_RING_SIZE] = {};

// ============================================================
// Theme
// ============================================================

const UITheme THEME_DARK = {
  0x0E1621,                                                  // screen bg
  0x2B5278, 0x182533, 0xF5F5F5, 0x8696A0, 0x5EB5F7,       // bubbles
  0x6DC264, 0xE05555, 0xF5A623, 0x6DC264,                  // status + signal (green)
  0x17212B, 0xF5F5F5,                                        // chat header
  0x17212B, 0x2B3B4D, 0xF5F5F5, 0x3390EC,                   // hs contact
  0x17212B, 0x2B3B4D, 0xF5F5F5, 0x6DC264, 0x3390EC,        // hs channel
  0xE05555,                                                   // badge
  0x2B3B4D, 0x3390EC, 0xE05555, 0x8696A0, 0xE05555,        // general
  0xF5A623,                                                   // new divider
  0x6DC264, 0xF5A623, 0xE05555, 0x6C7883,                   // signal bars
};

const UITheme* g_theme = &THEME_DARK;

// ---- Mesh globals ----
BaseChatMesh* g_mesh = nullptr;  // extern for extracted modules
#if defined(ESP32)
SemaphoreHandle_t g_mesh_mutex = nullptr;  // extern for cross-core mesh guard
static inline bool mesh_try_lock(TickType_t wait_ticks = 0) {
  if (!g_mesh_mutex) return true;
  return xSemaphoreTake(g_mesh_mutex, wait_ticks) == pdTRUE;
}
static inline void mesh_unlock() {
  if (g_mesh_mutex) xSemaphoreGive(g_mesh_mutex);
}
#endif
StdRNG fast_rng;
static uint32_t s_rx_max_service_gap_ms = 0;
static uint32_t s_rx_last_service_ms = 0;
static uint32_t s_rx_channel_accepted[MAX_GROUP_CHANNELS] = {};
static uint32_t s_rx_channel_last_epoch[MAX_GROUP_CHANNELS] = {};
static uint32_t s_rx_channel_duplicates = 0;
struct RxPathCounters {
  uint32_t raw[4] = {};
  uint32_t parsed[3] = {};
  uint32_t decrypted[3] = {};
  uint32_t accepted[3] = {};
  uint32_t short_metadata = 0;
  uint32_t unsupported_version = 0;
};
static RxPathCounters s_rx_paths;
static void report_rx_paths_if_changed();
SimpleMeshTables tables;


// ============================================================
// Duplicate message filter — ring buffer of recent msg fingerprints
// ============================================================
#define DEDUP_SLOTS 16
static uint32_t s_dedup_ring[DEDUP_SLOTS] = {};
static int      s_dedup_idx = 0;

// Simple hash: combine sender key bytes + timestamp + first text bytes
static uint32_t dedup_fingerprint(const uint8_t* pub_key, uint32_t ts, const char* text) {
  uint32_t h = 0x811c9dc5u;  // FNV-1a offset basis
  for (int i = 0; i < 8; i++) { h ^= pub_key[i]; h *= 0x01000193u; }
  h ^= ts; h *= 0x01000193u;
  for (int i = 0; text[i] && i < 32; i++) { h ^= (uint8_t)text[i]; h *= 0x01000193u; }
  return h;
}

static bool dedup_check_and_add(uint32_t fp) {
  for (int i = 0; i < DEDUP_SLOTS; i++) {
    if (s_dedup_ring[i] == fp) return true;  // duplicate
  }
  s_dedup_ring[s_dedup_idx] = fp;
  s_dedup_idx = (s_dedup_idx + 1) % DEDUP_SLOTS;
  return false;  // new
}

// ============================================================
// UIMesh class — MeshCore API surface
// ============================================================

class UIMesh : public BaseChatMesh, public ContactVisitor {
  FILESYSTEM* _fs;
  NodePrefs _prefs;
  ChannelDetails* _public;
  int _public_idx;
  ContactInfo* _curr_recipient;
  TargetKind _curr_kind;
  int _curr_channel_idx;
  uint32_t _pending_discovery = 0;
  TransportKey _send_scope;  // regional flood scope (all-zero = unrestricted)


  void loadPrefs() {
    memset(&_prefs, 0, sizeof(_prefs));
    _prefs.airtime_factor = 1.0f;
    strncpy(_prefs.node_name, "DHE04005D", sizeof(_prefs.node_name));
    _prefs.freq = (float)LORA_FREQ;
    _prefs.tx_power_dbm = (int8_t)LORA_TX_POWER;

    if (_fs->exists("/node_prefs")) {
#if defined(RP2040_PLATFORM)
      File file = _fs->open("/node_prefs", "r");
#else
      File file = _fs->open("/node_prefs");
#endif
      if (file) {
        file.read((uint8_t*)&_prefs, sizeof(_prefs));
        file.close();
      }
    }
    if (_prefs.path_hash_mode > 2) _prefs.path_hash_mode = 0;
    sanitize_ascii_inplace(_prefs.node_name);
  }

  void savePrefs() {
#if defined(NRF52_PLATFORM)
    _fs->remove("/node_prefs");
    File file = _fs->open("/node_prefs", FILE_O_WRITE);
#elif defined(RP2040_PLATFORM)
    File file = _fs->open("/node_prefs", "w");
#else
    File file = _fs->open("/node_prefs", "w", true);
#endif
    if (file) {
      file.write((const uint8_t *)&_prefs, sizeof(_prefs));
      file.close();
    }
  }

  bool contactsFileLooksComplete(const char* path) {
    if (!_fs || !_fs->exists(path)) return false;
#if defined(RP2040_PLATFORM)
    File file = _fs->open(path, "r");
#else
    File file = _fs->open(path);
#endif
    if (!file) return false;
    size_t size = file.size();
    file.close();
    return (size % sizeof(ContactRecord)) == 0;
  }

  void recoverContactsFile() {
    const char* contacts_path = "/contacts";
    const char* tmp_path = "/contacts.tmp";
    const char* bak_path = "/contacts.bak";

    if (contactsFileLooksComplete(contacts_path)) {
      if (_fs->exists(tmp_path)) _fs->remove(tmp_path);
      return;
    }

    if (contactsFileLooksComplete(bak_path)) {
      _fs->remove(contacts_path);
      if (_fs->rename(bak_path, contacts_path)) {
        if (_fs->exists(tmp_path)) _fs->remove(tmp_path);
        return;
      }
    }

    if (contactsFileLooksComplete(tmp_path)) {
      _fs->remove(contacts_path);
      _fs->rename(tmp_path, contacts_path);
    }
  }

  bool replaceContactsFile(const char* tmp_path) {
    const char* contacts_path = "/contacts";
    const char* bak_path = "/contacts.bak";

    crash_diagnostics_mark_current(CrashStage::Storage, 210);
    if (!contactsFileLooksComplete(tmp_path)) {
      _fs->remove(tmp_path);
      return false;
    }

    crash_diagnostics_mark_current(CrashStage::Storage, 211);
    _fs->remove(bak_path);
    crash_diagnostics_mark_current(CrashStage::Storage, 212);
    bool had_contacts = _fs->exists(contacts_path);
    crash_diagnostics_mark_current(CrashStage::Storage, 213);
    if (had_contacts && !_fs->rename(contacts_path, bak_path)) {
      _fs->remove(tmp_path);
      return false;
    }

    crash_diagnostics_mark_current(CrashStage::Storage, 214);
    if (_fs->rename(tmp_path, contacts_path)) {
      crash_diagnostics_mark_current(CrashStage::Storage, 217);
      return true;
    }

    crash_diagnostics_mark_current(CrashStage::Storage, 215);
    if (had_contacts && _fs->exists(bak_path)) {
      _fs->rename(bak_path, contacts_path);
    }
    crash_diagnostics_mark_current(CrashStage::Storage, 216);
    _fs->remove(tmp_path);
    return false;
  }

  void loadContacts() {
    recoverContactsFile();
    if (!_fs->exists("/contacts")) return;
#if defined(RP2040_PLATFORM)
    File file = _fs->open("/contacts", "r");
#else
    File file = _fs->open("/contacts");
#endif
    if (!file) return;

    ContactRecord rec;
    unsigned removed_placeholders = 0;
    while (file.read((uint8_t*)&rec, sizeof(rec)) == sizeof(rec)) {
      ContactInfo c{};
      c.id = mesh::Identity(rec.pub_key);
      memcpy(c.name, rec.name, 32);
      c.name[31] = '\0';
      sanitize_ascii_inplace(c.name);
      c.type   = rec.type;
      c.flags  = rec.flags;
      c.out_path_len          = rec.out_path_len;
      c.last_advert_timestamp = rec.last_advert_timestamp;
      memcpy(c.out_path, rec.out_path, 64);
      c.gps_lat = rec.gps_lat;
      c.gps_lon = rec.gps_lon;
      c.lastmod = 0;
      bool has_saved_route = c.out_path_len != OUT_PATH_UNKNOWN;
      for (uint8_t hop : c.out_path) has_saved_route = has_saved_route || hop != 0;
      if (NeighbourContactCleanup::shouldRemove(c.name, c.id.pub_key,
            c.type == ADV_TYPE_REPEATER, c.last_advert_timestamp,
            c.gps_lat, c.gps_lon, c.flags, has_saved_route)) {
        ++removed_placeholders;
        continue;
      }
      addContact(c);
    }
    file.close();
    if (removed_placeholders) {
      // Existing transactional save retains the original list as /contacts.bak.
      saveContacts();
      Serial.printf("[neighbours] removed %u unused imported placeholders; known contacts kept\n",
                    removed_placeholders);
    }
  }

  void saveContacts() {
    auto retry = []() { g_contacts_save_dirty = true; g_contacts_save_ms = millis(); };
    static std::atomic_flag saving = ATOMIC_FLAG_INIT;
    if (saving.test_and_set(std::memory_order_acquire)) { retry(); return; }
    struct SaveGuard {
      std::atomic_flag& flag;
      ~SaveGuard() { flag.clear(std::memory_order_release); }
    } guard{saving};
    // Snapshot under mesh ownership; never iterate the live table during I/O.
    // Keep bulk storage in PSRAM and stage flash writes through internal RAM.
    auto* snapshot = static_cast<ContactRecord*>(heap_caps_malloc(
        MAX_CONTACTS * sizeof(ContactRecord), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    constexpr size_t batch_records = 4;
    auto* staging = static_cast<uint8_t*>(heap_caps_malloc(
        batch_records * sizeof(ContactRecord), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!snapshot || !staging || !mesh_try_lock(pdMS_TO_TICKS(5))) {
      heap_caps_free(snapshot);
      heap_caps_free(staging);
      retry();
      return;
    }
    size_t count = 0;
    ContactsIterator iter = startContactsIterator();
    ContactInfo c;
    while (count < MAX_CONTACTS && iter.hasNext(this, c)) {
      ContactRecord& rec = snapshot[count++];
      memset(&rec, 0, sizeof(rec));
      sanitize_ascii_inplace(c.name);
      memcpy(rec.pub_key, c.id.pub_key, 32);
      memcpy(rec.name, c.name, 32);
      rec.type = c.type;
      rec.flags = c.flags;
      rec.out_path_len = c.out_path_len;
      rec.last_advert_timestamp = c.last_advert_timestamp;
      memcpy(rec.out_path, c.out_path, 64);
      rec.gps_lat = c.gps_lat;
      rec.gps_lon = c.gps_lon;
    }
    mesh_unlock();
    // Do not replace contacts while the UI is loading/pruning a chat file or
    // changing CPU/display state. Acquire only AFTER releasing mesh ownership.
    if (!lvgl_lock(5)) {
      heap_caps_free(snapshot);
      heap_caps_free(staging);
      retry();
      return;
    }
    struct RenderUnlock { ~RenderUnlock() { lvgl_unlock(); } } render_unlock;
    // Transactional save: write the new list to a temp file first, then keep
    // the previous contact database as /contacts.bak while swapping. If power
    // drops during the swap, boot recovery restores the backup instead of
    // starting with an empty contact/repeater list.
    const char* tmp_path = "/contacts.tmp";
    crash_diagnostics_mark_current(CrashStage::Storage, 200);
#if defined(NRF52_PLATFORM)
    _fs->remove(tmp_path);
    File file = _fs->open(tmp_path, FILE_O_WRITE);
#elif defined(RP2040_PLATFORM)
    File file = _fs->open(tmp_path, "w");
#else
    File file = _fs->open(tmp_path, "w", true);
#endif
    if (!file) {
      heap_caps_free(snapshot);
      heap_caps_free(staging);
      retry();
      return;
    }

    bool ok = true;
    for (size_t offset = 0; offset < count; offset += batch_records) {
      const size_t n = (count - offset < batch_records) ? count - offset : batch_records;
      const size_t bytes = n * sizeof(ContactRecord);
      memcpy(staging, snapshot + offset, bytes);
      crash_diagnostics_mark_current(CrashStage::Storage, 201);
      if (file.write(staging, bytes) != bytes) { ok = false; break; }
      vTaskDelay(1);
    }
    crash_diagnostics_mark_current(CrashStage::Storage, 202);
    file.close();
    heap_caps_free(snapshot);
    heap_caps_free(staging);

    if (!ok) {
      _fs->remove(tmp_path);   // partial write — leave the existing /contacts untouched
      retry();
      return;
    }
    crash_diagnostics_mark_current(CrashStage::Storage, 203);
    if (!replaceContactsFile(tmp_path)) {
      Serial.println("CONTACTS: save failed, kept previous /contacts");
      retry();
    }
  }

  void ui_set_devname() {
    if (ui_devicenamelabel) lv_label_set_text(ui_devicenamelabel, _prefs.node_name);
  }

  void saveChannelsToFS() {
#if defined(ESP32)
    File f = LittleFS.open(CHANNELS_FILE, FILE_WRITE);
    if (!f) return;

    uint8_t count = 0;
    f.write(&count, 1);

    ChannelDetails tmp;
    for (int i = 1; i < MAX_GROUP_CHANNELS; i++) {
      if (!getChannel(i, tmp) || tmp.name[0] == 0) break;
      f.write((const uint8_t*)tmp.name, 32);
      f.write((const uint8_t*)tmp.channel.secret, 16);
      count++;
    }

    f.seek(0);
    f.write(&count, 1);
    f.close();
#endif
  }

  void loadChannelsFromFS() {
#if defined(ESP32)
    File f = LittleFS.open(CHANNELS_FILE, FILE_READ);
    if (!f) return;

    uint8_t count = 0;
    if (f.read(&count, 1) != 1) { f.close(); return; }

    for (uint8_t i = 0; i < count; i++) {
      char name[32];
      uint8_t sec16[16];
      if (f.read((uint8_t*)name, 32) != 32) break;
      if (f.read(sec16, 16) != 16) break;
      name[31] = 0;
      sanitize_ascii_inplace(name);

      String b64 = secret16_to_base64(sec16);
      addChannel(name, b64.c_str());
    }
    f.close();
#endif
  }

protected:
  float getAirtimeBudgetFactor() const override { return _prefs.airtime_factor; }
  int calcRxDelay(float, uint32_t) const override { return 0; }
  void logRxRaw(float, float, const uint8_t raw[], int len) override {
    if (!raw || len < 1 || ((raw[0] >> PH_TYPE_SHIFT) & PH_TYPE_MASK) != PAYLOAD_TYPE_GRP_TXT) return;
    if (((raw[0] >> PH_VER_SHIFT) & PH_VER_MASK) > PAYLOAD_VER_1)
      ++s_rx_paths.unsupported_version;
    const uint8_t route = raw[0] & PH_ROUTE_MASK;
    const int path_offset = (route == ROUTE_TYPE_TRANSPORT_FLOOD ||
                             route == ROUTE_TYPE_TRANSPORT_DIRECT) ? 5 : 1;
    if (len <= path_offset) { ++s_rx_paths.short_metadata; return; }
    ++s_rx_paths.raw[raw[path_offset] >> 6];
  }
  void logRx(mesh::Packet* packet, int, float) override {
    if (packet->getPayloadType() != PAYLOAD_TYPE_GRP_TXT) return;
    const uint8_t width = packet->getPathHashSize();
    if (width >= 1 && width <= 3) ++s_rx_paths.parsed[width - 1];
  }
  void onGroupDataRecv(mesh::Packet* packet, uint8_t type, const mesh::GroupChannel& channel,
                       uint8_t* data, size_t len) override {
    if (type == PAYLOAD_TYPE_GRP_TXT) {
      const uint8_t width = packet->getPathHashSize();
      if (width >= 1 && width <= 3) ++s_rx_paths.decrypted[width - 1];
    }
    BaseChatMesh::onGroupDataRecv(packet, type, channel, data, len);
  }
  bool allowPacketForward(const mesh::Packet*) override { return g_packet_forward_enabled; }
  // Send one extra ACK ~300ms after the primary when a direct path is known.
  // Improves delivery-confirmation reliability on lossy return paths without
  // materially impacting airtime. Flood ACKs aren't duplicated (the ACK is
  // encoded in the path-return packet by BaseChatMesh).
  uint8_t getExtraAckTransmitCount() const override { return 1; }

  void sendFloodScoped(const ContactInfo& recipient, mesh::Packet* pkt, uint32_t delay_millis = 0) override {
    uint8_t phs = _prefs.path_hash_mode + 1;
    // Repeater control/login/path-discovery must stay globally routable.
    // If we attach transport scope codes here, nearby repeaters outside
    // that scope won't forward, which breaks long-hop repeater login.
    if (recipient.type == ADV_TYPE_REPEATER || _send_scope.isNull()) {
      sendFlood(pkt, delay_millis, phs);
    } else {
      uint16_t codes[2];
      codes[0] = _send_scope.calcTransportCode(pkt);
      codes[1] = 0;
      sendFlood(pkt, codes, delay_millis, phs);
    }
  }
  void sendFloodScoped(const mesh::GroupChannel& channel, mesh::Packet* pkt, uint32_t delay_millis = 0) override {
    uint8_t phs = _prefs.path_hash_mode + 1;
    if (_send_scope.isNull()) {
      sendFlood(pkt, delay_millis, phs);
    } else {
      uint16_t codes[2];
      codes[0] = _send_scope.calcTransportCode(pkt);
      codes[1] = 0;
      sendFlood(pkt, codes, delay_millis, phs);
    }
  }

  bool onContactPathRecv(ContactInfo& contact, uint8_t* in_path, uint8_t in_path_len,
                          uint8_t* out_path, uint8_t out_path_len,
                          uint8_t extra_type, uint8_t* extra, uint8_t extra_len) override {
    if (_pending_discovery && extra_type == PAYLOAD_TYPE_RESPONSE && extra_len > 4) {
      uint32_t tag;
      memcpy(&tag, extra, 4);
      if (tag == _pending_discovery) {
        _pending_discovery = 0;
        if (mesh::Packet::isValidPathLen(in_path_len) && mesh::Packet::isValidPathLen(out_path_len)) {
          char msg[128];
          snprintf(msg, sizeof(msg), "PATH DISCOVERY: in=%u out=%u for %s",
                   (unsigned)in_path_len, (unsigned)out_path_len, contact.name);
          serialmon_append_color(0x00FF80, msg);
        }
        // Let the base class store the discovered path (unlike companion which defers to the app)
        return BaseChatMesh::onContactPathRecv(contact, in_path, in_path_len,
                                                out_path, out_path_len, extra_type, extra, extra_len);
      }
    }
    return BaseChatMesh::onContactPathRecv(contact, in_path, in_path_len,
                                            out_path, out_path_len, extra_type, extra, extra_len);
  }

  void onControlDataRecv(mesh::Packet* packet) override {
    if (packet->payload_len < 6) return;
    uint8_t type = packet->payload[0] & 0xF0;
    uint8_t node_type = packet->payload[0] & 0x0F;

    if (type == 0x90 && node_type == ADV_TYPE_REPEATER) {  // CTL_TYPE_NODE_DISCOVER_RESP
      int8_t snr_x4 = (int8_t)packet->payload[1];
      uint32_t tag;
      memcpy(&tag, &packet->payload[2], 4);

      bool is_manual  = g_manual_discover_active && g_discover_tag != 0
                        && tag == g_discover_tag;
      bool is_receipt = (g_channel_receipt.discover_tag != 0 &&
                         tag == g_channel_receipt.discover_tag);
      if (!is_manual && !is_receipt) return;
      // Requests ask for the full identity; partial keys cannot identify a result safely.
      if (packet->payload_len < 6 + PUB_KEY_SIZE) return;

      // Extract pub_key from response
      uint8_t pub_key[PUB_KEY_SIZE];
      memset(pub_key, 0, sizeof(pub_key));
      size_t key_len = packet->payload_len - 6;
      if (key_len > PUB_KEY_SIZE) key_len = PUB_KEY_SIZE;
      memcpy(pub_key, &packet->payload[6], key_len);

      // Count unique responders for receipt discovery (counting only, no contact changes)
      if (is_receipt && g_channel_receipt.seen_count < 16) {
        bool dup = false;
        for (int i = 0; i < g_channel_receipt.seen_count; i++) {
          if (memcmp(g_channel_receipt.seen_keys[i], pub_key, 4) == 0) {
            dup = true; break;
          }
        }
        if (!dup) {
          memcpy(g_channel_receipt.seen_keys[g_channel_receipt.seen_count], pub_key, 4);
          g_channel_receipt.seen_count++;
        }
      }

      // For manual discover: add/update repeater contacts
      // For receipt discover: only counting above, no contact modifications
      if (is_manual) {
        ContactInfo* existing = lookupContactByPubKey(pub_key, PUB_KEY_SIZE);
        if (!existing) {
          // Brand new repeater — allocate a slot
          existing = allocateContactSlot();
          if (existing) {
            memset(existing, 0, sizeof(ContactInfo));
            memcpy(existing->id.pub_key, pub_key, PUB_KEY_SIZE);
            existing->type = ADV_TYPE_REPEATER;
            existing->out_path_len = 0;
            snprintf(existing->name, sizeof(existing->name), "Repeater_%02X%02X",
                     pub_key[0], pub_key[1]);
            existing->lastmod = rtc_clock.getCurrentTime();
            g_contacts_save_dirty = true; g_contacts_save_ms = millis();
          }
        } else if (existing->type == ADV_TYPE_REPEATER) {
          // Already known repeater — update path only
          existing->out_path_len = 0;
        }
        // NOTE: if existing contact is NOT a repeater, leave it untouched

        char fallback_name[32];
        snprintf(fallback_name, sizeof(fallback_name), "Repeater_%02X%02X", pub_key[0], pub_key[1]);
#if defined(ESP32)
        portENTER_CRITICAL(&s_discovery_mux);
#endif
        s_discovery_results.record(pub_key, existing ? existing->name : fallback_name,
                                   packet->_rssi, packet->_snr, snr_x4);
#if defined(ESP32)
        portEXIT_CRITICAL(&s_discovery_mux);
#endif
        if (existing) {
          g_deferred_refresh_targets = true;
          g_deferred_discover_done = true;
        }
      }
    }
  }

  bool shouldAutoAddContactType(uint8_t type) const override {
    if (type == ADV_TYPE_REPEATER) return g_auto_repeater_enabled || g_manual_discover_active;
    return g_auto_contact_enabled;
  }

  void onDiscoveredContact(ContactInfo& c, bool is_new, uint8_t, const uint8_t*) override {
    // Keep valid LoRa advert locations even when automatic contact addition is off.
    if (!g_purged_this_session && c.type == ADV_TYPE_REPEATER)
      map_record_repeater_advert(c.id.pub_key, c.name, c.gps_lat, c.gps_lon, c.last_advert_timestamp);
    // Persist advert updates to known repeaters even with auto-add off.
    if (!g_purged_this_session && c.type == ADV_TYPE_REPEATER && lookupContactByPubKey(c.id.pub_key, 32)) {
      g_contacts_save_dirty = true;
      g_contacts_save_ms = millis();
      g_deferred_refresh_targets = true;
    }
    {
      bool is_repeater = (c.type == ADV_TYPE_REPEATER);
      if (is_repeater  && !g_auto_repeater_enabled && !g_manual_discover_active) return;
      if (!is_repeater && !g_auto_contact_enabled)  return;
    }

    if (g_purged_this_session) return;

    c.flags &= ~CONTACT_FLAG_HIDDEN;
    sanitize_ascii_inplace(c.name);

    ContactInfo* ptr = lookupContactByPubKey(c.id.pub_key, 32);
    if (ptr) {
      ptr->flags &= ~CONTACT_FLAG_HIDDEN;
      ptr->type = c.type;
      StrHelper::strncpy(ptr->name, c.name, sizeof(ptr->name));
      sanitize_ascii_inplace(ptr->name);
    }

    g_contacts_save_dirty = true; g_contacts_save_ms = millis();
    if (c.type != ADV_TYPE_REPEATER) wake_on_event();

    char line[140];
    snprintf(line, sizeof(line), "ADVERT: %s type=%u", c.name, (unsigned)c.type);
    serialmon_append_color(0x00FFC8, line);
    g_deferred_refresh_targets = true;
  }

  void onContactPathUpdated(const ContactInfo& c) override {
    ContactInfo* ptr = lookupContactByPubKey(c.id.pub_key, 32);
    if (ptr) ptr->flags &= ~CONTACT_FLAG_HIDDEN;

    if (c.type != ADV_TYPE_REPEATER) wake_on_event();

    char line[192];
    OutboundPM* _pslot = pm_find_pending();
    bool pending_for_contact = _pslot && _pslot->recipient &&
                               memcmp(_pslot->recipient->id.pub_key, c.id.pub_key, 32) == 0;
    const char* route_str = (c.out_path_len == OUT_PATH_UNKNOWN) ? "flood" :
                             (c.out_path_len == 0) ? "direct" : "routed";
    snprintf(line, sizeof(line), "PATH updated: %s type=%u %s len=%u pending=%s key=%02X%02X%02X%02X",
      c.name, (unsigned)c.type, route_str,
      (unsigned)c.out_path_len,
      pending_for_contact ? "yes" : "no",
      c.id.pub_key[0], c.id.pub_key[1], c.id.pub_key[2], c.id.pub_key[3]);
    serialmon_append_color(0xFFB000, line);
    g_contacts_save_dirty = true; g_contacts_save_ms = millis();
    g_deferred_refresh_targets = true;

    if (g_in_chat_mode && _curr_kind == TargetKind::CONTACT && _curr_recipient &&
        memcmp(_curr_recipient->id.pub_key, c.id.pub_key, 32) == 0) {
      g_chat_route_path_len = c.out_path_len;
      g_deferred_route_label_dirty = true;
    }
  }

  ContactInfo* processAck(const uint8_t* data) override {
    uint32_t ack = 0;
    memcpy(&ack, data, 4);

    // Search ALL ring entries (pending + failed) for matching ACK
    OutboundPM* slot = pm_find_by_ack(ack);
    if (slot) {
      bool was_pending = (slot->state == 'P');
      serialmon_append(was_pending ? "TX delivered" : "TX delivered (late ACK)");
      pm_slot_mark_delivered(slot);
      return was_pending ? slot->recipient : nullptr;
    }
    return nullptr;
  }

  void onMessageRecv(const ContactInfo& contact, mesh::Packet* pkt, uint32_t ts, const char* text) override {
    wake_on_event();
    if (pkt) snr_contact_update(contact.id.pub_key, pkt->_snr);

    int hops = pkt ? (int)pkt->getPathHashCount() : -1;

    bool is_active = (g_in_chat_mode && _curr_kind == TargetKind::CONTACT && _curr_recipient &&
                      memcmp(_curr_recipient->id.pub_key, contact.id.pub_key, 32) == 0);

    // Learn direct path from incoming PM: only for 0-hop (no path bytes needed).
    // Multi-hop path learning is handled by MeshCore via onContactPathUpdated.
    // This runs BEFORE dedup so a duplicate arriving via direct still updates the route.
    ContactInfo* ci = lookupContactByPubKey(contact.id.pub_key, 32);
    if (ci && pkt && hops == 0 && ci->out_path_len != 0) {
      ci->out_path_len = 0;
      g_contacts_save_dirty = true;
      g_contacts_save_ms = millis();
      if (is_active) {
        g_chat_route_path_len = 0;
        g_deferred_route_label_dirty = true;
      }
    }

    // Deduplicate: skip if we've already processed this exact message.
    // Path learning above still runs so route labels stay accurate.
    uint32_t fp = dedup_fingerprint(contact.id.pub_key, ts, text ? text : "");
    if (dedup_check_and_add(fp)) {
#if SERIALMON_VERBOSE
      serialmon_append("RX PM duplicate - ignored");
#endif
      return;
    }

    String safeName = sanitize_ascii_string(contact.name);
    String safeText = sanitize_ascii_string(text);

    char logline[260];
    const char* route = (pkt && pkt->isRouteFlood()) ? "FLOOD" : "DIRECT";
    if (hops >= 0) snprintf(logline, sizeof(logline), "RX %s hops=%d %s: %s", route, hops, safeName.c_str(), safeText.c_str());
    else           snprintf(logline, sizeof(logline), "RX %s hops=? %s: %s", route, safeName.c_str(), safeText.c_str());
    serialmon_append(logline);

    String sig = packet_signal_str(pkt);

    char bubble[512];
    snprintf(bubble, sizeof(bubble), "[%s] %s: %s",
             time_string_now().c_str(), safeName.c_str(), safeText.c_str());
    append_chat_to_file(key_for_contact(contact.id), false, bubble, 0, sig.c_str());

    // Translate immediately on receive (when auto-translate + WiFi are on),
    // so the translation lands in the bubble before the user opens the chat.
    // Without this, the on-chat-open backfill would queue a translate for
    // every untranslated RX line — a long backlog floods the 6-slot queue
    // and crashes the device on low memory.
    if (g_auto_translate_enabled && g_wifi_connected && safeText.length()) {
        translate_request_to_file(safeText.c_str(),
                                  key_for_contact(contact.id).c_str());
    }

    char target_key[11];
    snprintf(target_key, sizeof(target_key), "c:%02x%02x%02x%02x",
             contact.id.pub_key[0], contact.id.pub_key[1],
             contact.id.pub_key[2], contact.id.pub_key[3]);

    // Forward to bridges (incoming PM: sender=contact, recipient=me)
    tgbridge_forward_pm(safeName.c_str(), _prefs.node_name, safeText.c_str(), false);
    webdash_broadcast_message(safeName.c_str(), safeText.c_str(), false, target_key);

    // Auto-translate incoming PM whenever Wi-Fi is up. Not gated on
    // g_auto_translate_enabled anymore — user preference is to
    // always translate when online, for any chat or channel. The
    // Auto-Translate toggle in Web Apps is now effectively a
    // cosmetic no-op; it can be removed in a future cleanup pass.
    if (g_auto_translate_enabled && g_wifi_connected) {
      String ck = key_for_contact(contact.id);
      translate_request_to_file(safeText.c_str(), ck.c_str());
    }

    if (is_active) {
      deferred_msg_push(false, bubble, sig.c_str());
    } else {
      notify_contact_inc(contact.id.pub_key);
      g_deferred_refresh_targets = true;
    }
    g_deferred_sound = 1;
  }

  void onCommandDataRecv(const ContactInfo& contact, mesh::Packet*, uint32_t, const char* text) override {
    map_receive_repeater_position(contact.id.pub_key, text);
    wake_on_event();
    String safeName = sanitize_ascii_string(contact.name);
    String safeText = sanitize_ascii_string(text);

    char logline[260];
    snprintf(logline, sizeof(logline), "RX CMD %s: %s", safeName.c_str(), safeText.c_str());
    serialmon_append(logline);

    // If the user is in CLI mode on the Repeaters screen and this
    // CLI_DATA reply is from the repeater they're talking to, append
    // it to the on-screen terminal.
    if (repeater_cli_mode_active()) {
      repeater_cli_handle_response(contact.id.pub_key, safeText.c_str());
    }
  }

  void onSignedMessageRecv(const ContactInfo& contact, mesh::Packet* pkt, uint32_t ts, const uint8_t*, const char* text) override {
    onMessageRecv(contact, pkt, ts, text);
  }

  uint32_t calcFloodTimeoutMillisFor(uint32_t air) const override {
    return (uint32_t)(SEND_TIMEOUT_BASE_MILLIS + air * FLOOD_SEND_TIMEOUT_FACTOR);
  }

  uint32_t calcDirectTimeoutMillisFor(uint32_t air, uint8_t path_len) const override {
    uint8_t path_hash_count = path_len & 63;
    return (uint32_t)(SEND_TIMEOUT_BASE_MILLIS +
           (air * DIRECT_SEND_PERHOP_FACTOR + DIRECT_SEND_PERHOP_EXTRA_MILLIS) *
           (path_hash_count + 1));
  }

  // --- Ring buffer helpers ---
  static OutboundPM* pm_find_pending() {
    for (int i = 0; i < PM_RING_SIZE; i++)
      if (g_pm_ring[i].active && g_pm_ring[i].state == 'P') return &g_pm_ring[i];
    return nullptr;
  }

  static void pm_fail_existing_pending(const char* reason) {
    OutboundPM* prev = pm_find_pending();
    if (!prev) return;
    serialmon_append(reason ? reason : "TX PM: superseding previous pending message");
    pm_slot_mark_failed(prev);
    if (prev->status_label) {
      apply_status_to_label(prev->status_label, 'N');
      prev->ui_dirty = false;
    }
  }

  static OutboundPM* pm_find_by_ack(uint32_t ack) {
    for (int i = 0; i < PM_RING_SIZE; i++) {
      if (!g_pm_ring[i].active) continue;
      for (int j = 0; j < g_pm_ring[i].ack_count; j++)
        if (g_pm_ring[i].ack_codes[j] == ack) return &g_pm_ring[i];
    }
    return nullptr;
  }

  static OutboundPM* pm_find_by_msg_ts(uint32_t msg_ts) {
    if (!msg_ts) return nullptr;
    for (int i = 0; i < PM_RING_SIZE; i++) {
      if (g_pm_ring[i].active && g_pm_ring[i].msg_ts == msg_ts) return &g_pm_ring[i];
    }
    return nullptr;
  }

  static OutboundPM* pm_alloc_slot() {
    // First try an inactive slot
    for (int i = 0; i < PM_RING_SIZE; i++)
      if (!g_pm_ring[i].active) return &g_pm_ring[i];
    // Evict the oldest non-pending (failed/delivered) entry
    uint32_t oldest_ms = UINT32_MAX;
    int oldest_idx = -1;
    for (int i = 0; i < PM_RING_SIZE; i++) {
      if (g_pm_ring[i].state == 'P') continue; // don't evict pending
      if (g_pm_ring[i].expiry_ms < oldest_ms) {
        oldest_ms = g_pm_ring[i].expiry_ms;
        oldest_idx = i;
      }
    }
    if (oldest_idx >= 0) return &g_pm_ring[oldest_idx];
    // Worst case: evict slot 0
    return &g_pm_ring[0];
  }

  static void pm_slot_mark_failed(OutboundPM* slot) {
    if (!slot) return;
    slot->state = 'N';
    slot->ui_dirty = true;
    slot->file_dirty = true;
    slot->hard_timeout_ms = 0;
    slot->retry_timeout_ms = 0;
    slot->expiry_ms = millis() + PM_LATE_EXPIRY_MS;
  }

  // After all retries exhausted without ACK: show "Probably delivered" for a
  // window. A late ACK arriving during this window flips to delivered (green).
  // If the window expires, checkPmUnconfirmedExpiry() transitions to 'N'.
  static void pm_slot_mark_unconfirmed(OutboundPM* slot) {
    if (!slot) return;
    slot->state = 'U';
    slot->ui_dirty = true;
    slot->file_dirty = true;
    slot->hard_timeout_ms = 0;
    slot->retry_timeout_ms = 0;
    slot->expiry_ms = millis() + PM_UNCONFIRMED_MS;
  }

  static void pm_slot_mark_delivered(OutboundPM* slot) {
    if (!slot) return;
    slot->state = 'D';
    slot->ui_dirty = true;
    slot->file_dirty = true;
    slot->hard_timeout_ms = 0;
    slot->retry_timeout_ms = 0;
    slot->expiry_ms = millis() + 5000; // keep briefly then evict
  }

  static void pm_evict_expired() {
    uint32_t now = millis();
    for (int i = 0; i < PM_RING_SIZE; i++) {
      OutboundPM& s = g_pm_ring[i];
      // 'U' (unconfirmed) slots are handled separately by checkPmUnconfirmedExpiry,
      // which transitions them to 'N' when the window expires (so the user can press Resend).
      if (s.active && s.state != 'P' && s.state != 'U' && s.expiry_ms && (int32_t)(now - s.expiry_ms) > 0) {
        s.active = false;
        s.status_label = nullptr;
      }
    }
  }


  // pm_mark_failed now operates on the pending ring slot
  void pm_mark_current_failed() {
    OutboundPM* slot = pm_find_pending();
    if (slot) pm_slot_mark_failed(slot);
  }

  static const uint8_t PM_TOTAL_ATTEMPTS = 3;
  static const uint8_t MAX_PM_RETRIES = PM_TOTAL_ATTEMPTS - 1;
  static const uint8_t FLOOD_FALLBACK_RETRY = 2;  // retry 1 uses stored path; retry 2 forces flood
  static const uint32_t PM_HARD_TIMEOUT_MS = 15000UL + 12000UL + 10000UL + 3000UL;

  static uint32_t pm_timeout_for_attempt(uint8_t attempt_number) {
    switch (attempt_number) {
      case 1: return 15000UL;
      case 2: return 12000UL;
      default: return 10000UL;
    }
  }

  static void pm_apply_sending_label(lv_obj_t* lbl, uint8_t attempt_number) {
    if (!lbl) return;
    if (attempt_number <= 1) {
      lv_label_set_text(lbl, "Sending");
    } else {
      char buf[40];
      snprintf(buf, sizeof(buf), "Sending (attempt %u/%u)",
               (unsigned)attempt_number, (unsigned)PM_TOTAL_ATTEMPTS);
      lv_label_set_text(lbl, buf);
    }
    lv_obj_set_style_text_color(lbl, lv_color_hex(g_theme->bubble_text), 0);
    lv_obj_set_style_text_opa(lbl, LV_OPA_50, 0);
  }

  // Core retry logic — shared by onSendTimeout (first timeout from base class)
  // and checkPmRetryTimeout (subsequent retries via our own timer).
  void doPmRetry(OutboundPM* slot) {
    if (slot->retry_count < MAX_PM_RETRIES && slot->recipient && slot->retry_text[0]) {
      slot->retry_count++;
      uint8_t attempt_number = slot->retry_count + 1;
      bool force_flood = (slot->retry_count >= FLOOD_FALLBACK_RETRY);
      const char* via = force_flood ? "flood" : "stored path";
      char msg[96];
      snprintf(msg, sizeof(msg), "TX timeout, attempt %u/%u via %s (stored path=%u)",
               (unsigned)attempt_number, (unsigned)PM_TOTAL_ATTEMPTS,
               via, (unsigned)slot->recipient->out_path_len);
      serialmon_append(msg);

      if (slot->status_label) {
        pm_apply_sending_label(slot->status_label, attempt_number);
      }

      uint32_t expected_ack = 0, est_timeout = 0;
      int result;
      if (force_flood) {
        ContactInfo flood_target = *(slot->recipient);
        flood_target.out_path_len = OUT_PATH_UNKNOWN;
        result = sendMessage(flood_target, slot->msg_ts, slot->retry_count, slot->retry_text, expected_ack, est_timeout);
      } else {
        result = sendMessage(*(slot->recipient), slot->msg_ts, slot->retry_count, slot->retry_text, expected_ack, est_timeout);
      }
      if (result != MSG_SEND_FAILED) {
        if (slot->ack_count < MAX_PM_ACK_CODES)
          slot->ack_codes[slot->ack_count++] = expected_ack;
        slot->retry_timeout_ms = millis() + pm_timeout_for_attempt(attempt_number);
        return;
      }
    }
    // Retries exhausted: enter 'Probably delivered' state for PM_UNCONFIRMED_MS.
    // A late ACK during this window flips to 'D' (delivered). Otherwise the
    // slot transitions to 'N' (Failed) in checkPmUnconfirmedExpiry().
    if (slot->recipient) {
      char diag[128];
      snprintf(diag, sizeof(diag),
               "PM retries exhausted: stored path=%u, acks_tracked=%d, awaiting late ACK",
               (unsigned)slot->recipient->out_path_len, (int)slot->ack_count);
      serialmon_append(diag);

      if (slot->recipient->out_path_len != OUT_PATH_UNKNOWN) {
        slot->recipient->out_path_len = OUT_PATH_UNKNOWN;
        g_contacts_save_dirty = true;
        g_contacts_save_ms = millis();
        serialmon_append("Auto-reset path to flood after retries");

        if (g_in_chat_mode && _curr_recipient == slot->recipient) {
          g_chat_route_path_len = OUT_PATH_UNKNOWN;
          g_deferred_route_label_dirty = true;
        }
      }
    } else {
      serialmon_append("PM retries exhausted: no recipient");
    }
    pm_slot_mark_unconfirmed(slot);
  }

  void onSendTimeout() override {
    OutboundPM* slot = pm_find_pending();
    if (!slot) return;
    // If retry_timeout_ms is active, our own timer is managing retries —
    // ignore this base class txt_send_timeout fire to prevent double-retry.
    if (slot->retry_timeout_ms) return;
    doPmRetry(slot);
  }

  void onChannelMessageRecv(const mesh::GroupChannel& channel, mesh::Packet* pkt, uint32_t ts, const char *text) override {
    // Deduplicate channel messages
    uint32_t fp = dedup_fingerprint(channel.hash, ts, text ? text : "");
    if (dedup_check_and_add(fp)) {
      ++s_rx_channel_duplicates;
#if SERIALMON_VERBOSE
      serialmon_append("RX CH duplicate - ignored");
#endif
      return;
    }

    int idx = findChannelIdx(channel);
    bool muted = (idx >= 0 && idx < 32 && (g_muted_channel_mask & (1u << idx)));
    if (!muted) wake_on_event();
    if (idx < 0) return;

    ChannelDetails ch;
    if (!getChannel(idx, ch)) return;

    if (idx < MAX_GROUP_CHANNELS) {
      ++s_rx_channel_accepted[idx];
      s_rx_channel_last_epoch[idx] = rtc_clock.getCurrentTime();
    }
    if (pkt && pkt->getPathHashSize() >= 1 && pkt->getPathHashSize() <= 3)
      ++s_rx_paths.accepted[pkt->getPathHashSize() - 1];

    String safeCh = sanitize_ascii_string(ch.name);
    String safeText = sanitize_ascii_string(text);

    int hops = pkt ? (int)pkt->getPathHashCount() : -1;
    char logline[300];
    if (hops >= 0) snprintf(logline, sizeof(logline), "RX CH hops=%d %s: %s", hops, safeCh.c_str(), safeText.c_str());
    else           snprintf(logline, sizeof(logline), "RX CH hops=? %s: %s", safeCh.c_str(), safeText.c_str());
    serialmon_append(logline);

    String sig = packet_signal_str(pkt);

    char bubble[512];
    snprintf(bubble, sizeof(bubble), "[%s] %s", time_string_now().c_str(), safeText.c_str());
    append_chat_to_file(key_for_channel(idx), false, bubble, 0, sig.c_str());

    // Translate immediately on receive (see PM RX path for rationale). Strip
    // the "SenderName: " prefix that channel messages carry so we only push
    // the body to Google Translate, matching the on-chat-open backfill.
    if (g_auto_translate_enabled && g_wifi_connected && safeText.length()) {
        int sep_tr = safeText.indexOf(": ");
        String body_tr = (sep_tr > 0) ? safeText.substring(sep_tr + 2) : safeText;
        if (body_tr.length()) {
            translate_request_to_file(body_tr.c_str(),
                                      key_for_channel(idx).c_str());
        }
    }

    // Split "SenderName: message" for Telegram bridge
    {
      int sep = safeText.indexOf(": ");
      if (sep > 0) {
        String chSender = safeText.substring(0, sep);
        String chMsg = safeText.substring(sep + 2);
        tgbridge_forward_channel(safeCh.c_str(), chSender.c_str(), chMsg.c_str());
      } else {
        tgbridge_forward_channel(safeCh.c_str(), "?", safeText.c_str());
      }
    }
    char target_key[12];
    snprintf(target_key, sizeof(target_key), "h:%d", idx);
    webdash_broadcast_message(safeCh.c_str(), safeText.c_str(), false, target_key);

    // Auto-translate incoming channel messages whenever Wi-Fi is up
    // — not gated on g_auto_translate_enabled. Strip the
    // "SenderName: " prefix so the translator gets the message body
    // alone; the bubble's body label has the same bare text so the
    // match-by-text lookup in translate_loop succeeds.
    if (g_auto_translate_enabled && g_wifi_connected) {
      String ck = key_for_channel(idx);
      int sep2 = safeText.indexOf(": ");
      const char* body_to_translate = (sep2 > 0) ? safeText.c_str() + sep2 + 2 : safeText.c_str();
      translate_request_to_file(body_to_translate, ck.c_str());
    }

    bool is_active = (g_in_chat_mode && _curr_kind == TargetKind::CHANNEL && _curr_channel_idx == idx);

    if (is_active) {
      deferred_msg_push(false, bubble, sig.c_str());
    } else if (!muted) {
      notify_channel_inc(idx);
      g_deferred_refresh_targets = true;
    }
    if (!muted) g_deferred_sound = 1;
  }

  uint8_t onContactRequest(const ContactInfo&, uint32_t, const uint8_t*, uint8_t, uint8_t*) override { return 0; }

  void onContactResponse(const ContactInfo& contact, const uint8_t* data, uint8_t len) override {
    if (len < 4) return;

    {
      char dbg[64];
      snprintf(dbg, sizeof(dbg), "RESP: key=%02X%02X%02X%02X len=%d d4=0x%02X",
        contact.id.pub_key[0], contact.id.pub_key[1],
        contact.id.pub_key[2], contact.id.pub_key[3],
        len, len >= 5 ? data[4] : 0);
      serialmon_append_color(0xFF8C00, dbg);
    }

    // ---- Login response ----
    if (memcmp(g_login_pending_key, contact.id.pub_key, 4) == 0 &&
        (g_login_pending_key[0] | g_login_pending_key[1] |
         g_login_pending_key[2] | g_login_pending_key[3]) != 0) {

      memset(g_login_pending_key, 0, 4);

      bool ok = false;
      if (len >= 6 && memcmp(&data[4], "OK", 2) == 0) ok = true;
      else if (len >= 5 && data[4] == 0) ok = true;

      if (ok) {
        g_repeater_logged_in = true;
        g_deferred_repeater_btns = 1;

        {
          uint16_t keep_alive_secs = (len >= 6 && data[4] == 0 && data[5] > 0)
                                     ? (uint16_t)(data[5]) * 16
                                     : 60;
          ContactInfo* stored = lookupContactByPubKey(contact.id.pub_key, 32);
          if (stored && stored->out_path_len >= 0)
            startConnection(*stored, keep_alive_secs);
        }

        uint32_t tag, est_timeout;
        sendRequest(contact, (uint8_t)0x01, tag, est_timeout);
        g_status_pending_key = tag;
        char msg[80];
        snprintf(msg, sizeof(msg), "Login OK - %s\nRequesting status...", contact.name);
        repeater_update_monitor(msg);
      } else {
        g_repeater_logged_in = false;
        repeater_update_monitor("Login FAILED - wrong password or not authorised.");
      }
      return;
    }

    // ---- Status response ----
    if (g_status_pending_key != 0 && len >= 4 + (int)sizeof(RepeaterStats)) {
      uint32_t tag; memcpy(&tag, data, 4);
      bool match = (tag == g_status_pending_key) ||
                   (memcmp(&g_status_pending_key, contact.id.pub_key, 4) == 0);
      if (match) {
        g_status_pending_key = 0;
        RepeaterStats s;
        memcpy(&s, &data[4], sizeof(s));

        float batt_v   = s.batt_milli_volts / 1000.0f;
        int   batt_pct = (int)((batt_v - 3.0f) / (4.2f - 3.0f) * 100.0f);
        if (batt_pct < 0) batt_pct = 0;
        if (batt_pct > 100) batt_pct = 100;

        float snr_f    = s.last_snr / 4.0f;

        char buf[600];
        snprintf(buf, sizeof(buf),
          "Battery:\n"
          "  %d%%  /  %.2fV\n\n"
          "Uptime:\n"
          "  %s\n\n"
          "Total Airtime:\n"
          "  Tx: %s\n"
          "  Rx: %s\n\n"
          "Last RSSI:\n"
          "  %d dBm\n\n"
          "Last SNR:\n"
          "  %.1f dB\n\n"
          "Noise Floor:\n"
          "  %d dBm\n\n"
          "Packets Sent:\n"
          "  Total: %lu,  Flood: %lu,  Direct: %lu\n\n"
          "Packets Received:\n"
          "  Total: %lu,  Flood: %lu,  Direct: %lu\n\n"
          "Received Packet Errors:\n"
          "  %lu",
          batt_pct, batt_v,
          fmt_duration(s.total_up_time_secs).c_str(),
          fmt_duration(s.total_air_time_secs).c_str(),
          fmt_duration(s.total_rx_air_time_secs).c_str(),
          (int)s.last_rssi,
          snr_f,
          (int)s.noise_floor,
          (unsigned long)s.n_packets_sent,
          (unsigned long)s.n_sent_flood,
          (unsigned long)s.n_sent_direct,
          (unsigned long)s.n_packets_recv,
          (unsigned long)s.n_recv_flood,
          (unsigned long)s.n_recv_direct,
          (unsigned long)s.n_recv_errors
        );
        repeater_update_monitor(buf);
        return;
      }
    }

    // ---- Channel receipt responses now handled via onControlDataRecv (multi-repeater) ----

    // ---- Neighbours response ----
    map_receive_neighbours(contact.id.pub_key, data, len);
    if (g_neighbours_pending_key != 0 && len >= 8) {
      uint32_t tag; memcpy(&tag, data, 4);
      bool match = (tag == g_neighbours_pending_key) ||
                   (memcmp(&g_neighbours_pending_key, contact.id.pub_key, 4) == 0);
      if (match) {
        g_neighbours_pending_key = 0;

        uint16_t total_count, result_count;
        memcpy(&total_count,  &data[4], 2);
        memcpy(&result_count, &data[6], 2);

        char buf[512];
        int pos = 0;
        pos += snprintf(buf + pos, sizeof(buf) - pos,
          "Neighbours (%u of %u):\n\n", result_count, total_count);

        const uint8_t* p = &data[8];
        const uint8_t* end = data + len;
        int entry_sz = 4 + 4 + 1;

        if (result_count == 0) {
          pos += snprintf(buf + pos, sizeof(buf) - pos, "  (none)");
        }

        for (int i = 0; i < result_count && end - p >= entry_sz && pos < (int)sizeof(buf) - 1; i++, p += entry_sz) {
          uint32_t heard_ago; memcpy(&heard_ago, p + 4, 4);
          int8_t snr_raw; memcpy(&snr_raw, p + 8, 1);
          float snr_f = snr_raw / 4.0f;

          ContactInfo* ci = lookupContactByPubKey(p, 4);
          if (ci) {
            char safe[36]; strncpy(safe, ci->name, 35); safe[35] = 0;
            pos += snprintf(buf + pos, sizeof(buf) - pos,
              "%d. %s\n   SNR: %.1f  |  heard %lus ago\n\n",
              i + 1, safe, snr_f, (unsigned long)heard_ago);
          } else {
            pos += snprintf(buf + pos, sizeof(buf) - pos,
              "%d. %02X%02X%02X%02X...\n   SNR: %.1f  |  heard %lus ago\n\n",
              i + 1, p[0], p[1], p[2], p[3], snr_f, (unsigned long)heard_ago);
          }
          // snprintf returns the required length, not the bytes actually written.
          if (pos >= (int)sizeof(buf)) pos = sizeof(buf) - 1;
        }
        repeater_update_monitor(buf);
      }
    }
  }

public:
  static constexpr int kRadioPacketPoolSize = 16;


  bool hasPendingRadioPackets() const {
    // Includes delayed inbound/manual-held packets, not just queued TX.
    return _mgr->getFreeCount() != kRadioPacketPoolSize;
  }

  UIMesh(mesh::Radio& radio, mesh::RNG& rng, mesh::RTCClock& rtc, mesh::MeshTables& tables)
    : BaseChatMesh(radio, *new ArduinoMillis(), rng, rtc,
                   *(new StaticPoolPacketManager(kRadioPacketPoolSize)), tables) {
    _fs = nullptr;
    _public = nullptr;
    _public_idx = -1;
    _curr_recipient  = nullptr;
    _curr_kind       = TargetKind::NONE;
    _curr_channel_idx= -1;
    memset(&_send_scope, 0, sizeof(_send_scope));  // default: unscoped flood
    // g_pm_ring is a global zero-initialized array; no memset needed
  }

  void begin(FILESYSTEM& fs) {
    _fs = &fs;
    BaseChatMesh::begin();

    loadPrefs();

#if defined(NRF52_PLATFORM)
    IdentityStore store(fs, "");
#elif defined(RP2040_PLATFORM)
    IdentityStore store(fs, "/identity");
    store.begin();
#else
    IdentityStore store(fs, "/identity");
#endif

    char ident_name[32] = {0};
    StrHelper::strncpy(ident_name, _prefs.node_name, sizeof(ident_name));

    bool have_identity = store.load("_main", self_id, ident_name, sizeof(ident_name));

    if (!have_identity) {
      self_id = mesh::LocalIdentity(getRNG());
      int tries = 0;
      while (tries < 10 && (self_id.pub_key[0] == 0x00 || self_id.pub_key[0] == 0xFF)) {
        self_id = mesh::LocalIdentity(getRNG());
        tries++;
      }
      store.save("_main", self_id, _prefs.node_name);
      StrHelper::strncpy(ident_name, _prefs.node_name, sizeof(ident_name));
      serialmon_append("Identity created (fresh)");
      have_identity = true;
    } else {
      serialmon_append("Identity loaded");
    }

    char nvs_name[32] = {0};
    bool have_nvs = load_device_name_nvs(nvs_name, sizeof(nvs_name));

    if (have_nvs && nvs_name[0]) {
      StrHelper::strncpy(_prefs.node_name, nvs_name, sizeof(_prefs.node_name));
    } else if (ident_name[0]) {
      StrHelper::strncpy(_prefs.node_name, ident_name, sizeof(_prefs.node_name));
      save_device_name_nvs(_prefs.node_name);
    } else {
      save_device_name_nvs(_prefs.node_name);
    }
    sanitize_ascii_inplace(_prefs.node_name);

    if (have_identity && strncmp(ident_name, _prefs.node_name, sizeof(_prefs.node_name)) != 0) {
      store.save("_main", self_id, _prefs.node_name);
    }

    savePrefs();

    loadContacts();

    _public = addChannel("Public", PUBLIC_GROUP_PSK);
    _public_idx = _public ? findChannelIdx(_public->channel) : -1;

    loadChannelsFromFS();

    ui_set_devname();
  }

  int getPublicChannelIdx() const { return _public_idx; }

  const char* getNodeName() const { return _prefs.node_name; }
  float getFreqPref() const { return _prefs.freq; }
  int8_t getTxPowerPref() const { return _prefs.tx_power_dbm; }
  void setTxPowerPref(int8_t dbm) { _prefs.tx_power_dbm = dbm; savePrefs(); }
  void setPathHashBytes(uint8_t bytes) {
    uint8_t mode = (bytes <= 1) ? 0 : (bytes == 2) ? 1 : 2;  // prefs stores 0..2
    if (_prefs.path_hash_mode != mode) {
      _prefs.path_hash_mode = mode;
      savePrefs();
    }
  }
  uint8_t getPathHashBytes() const {
    uint8_t mode = _prefs.path_hash_mode;
    if (mode > 2) mode = 0;
    return (uint8_t)(mode + 1);
  }
  double getNodeLat() const { return _prefs.node_lat; }
  double getNodeLon() const { return _prefs.node_lon; }
  void setNodePosition(double lat, double lon) {
    _prefs.node_lat = lat;
    _prefs.node_lon = lon;
    savePrefs();
  }
  void clearNodePosition() { setNodePosition(0.0, 0.0); }

  void renameIfNonEmpty(const char* new_name) {
    if (!new_name) return;
    while (*new_name == ' ') new_name++;
    if (!*new_name) return;

    StrHelper::strncpy(_prefs.node_name, new_name, sizeof(_prefs.node_name));
    sanitize_ascii_inplace(_prefs.node_name);
    savePrefs();
    save_device_name_nvs(_prefs.node_name);

#if defined(NRF52_PLATFORM)
    IdentityStore store(*_fs, "");
#elif defined(RP2040_PLATFORM)
    IdentityStore store(*_fs, "/identity");
    store.begin();
#else
    IdentityStore store(*_fs, "/identity");
#endif
    store.save("_main", self_id, _prefs.node_name);

    ui_set_devname();
    serialmon_append("Device renamed");
    // Previously auto-issued a flood advert so the mesh learned the
    // new name immediately. Removed per user preference: renaming
    // shouldn't spam the airwaves. The new name still propagates via
    // the next scheduled advert (flood / zero-hop buttons, or
    // ENABLE_ADVERT_ON_BOOT on next reboot).
  }

  void sendFloodAdvert(int delay_ms=0) {
#if SERIALMON_VERBOSE
    serialmon_append("TX flood advert");
#endif
    auto pkt = g_position_advert_enabled
      ? createSelfAdvert(_prefs.node_name, _prefs.node_lat, _prefs.node_lon)
      : createSelfAdvert(_prefs.node_name);
    if (pkt) sendFlood(pkt, delay_ms);
  }

  void sendZeroHopAdvert(int delay_ms=0) {
#if SERIALMON_VERBOSE
    serialmon_append("TX 0-hop advert");
#endif
    auto pkt = g_position_advert_enabled
      ? createSelfAdvert(_prefs.node_name, _prefs.node_lat, _prefs.node_lon)
      : createSelfAdvert(_prefs.node_name);
    if (pkt) sendZeroHop(pkt, delay_ms);
  }

  uint32_t sendDiscoverRepeaters(bool flood = false, uint32_t delay_ms = 0) {
    uint32_t tag = (uint32_t)millis() ^ ((uint32_t)random(0xFFFF) << 16);
    if (!tag) tag = 1;  // Zero denotes an inactive scan.
    uint8_t data[10];
    data[0] = 0x80;  // CTL_TYPE_NODE_DISCOVER_REQ, no prefix_only flag
    data[1] = (1 << ADV_TYPE_REPEATER);  // filter: repeaters only
    memcpy(&data[2], &tag, 4);
    uint32_t since = 0;  // all repeaters
    memcpy(&data[6], &since, 4);

    auto pkt = createControlData(data, sizeof(data));
    if (pkt) {
      if (flood) {
        sendFlood(pkt, delay_ms, _prefs.path_hash_mode + 1);
      } else {
        sendZeroHop(pkt, delay_ms);
      }
      return tag;
    }
    return 0;
  }

  TargetKind currentKind() const { return _curr_kind; }
  int currentChannelIdx() const { return _curr_channel_idx; }
  ContactInfo* currentRecipient() const { return _curr_recipient; }
  bool isPmPending() const {
    for (int i = 0; i < PM_RING_SIZE; i++)
      if (g_pm_ring[i].active && g_pm_ring[i].state == 'P') return true;
    return false;
  }
  void saveContactsNow() { saveContacts(); }

  bool resetRepeaterPathByPubKey(const uint8_t* pub_key) {
    if (!pub_key) return false;
    ContactInfo* c = lookupContactByPubKey(pub_key, 32);
    if (!c || c->type != ADV_TYPE_REPEATER) return false;
    c->out_path_len = OUT_PATH_UNKNOWN;
    memset(c->out_path, 0, sizeof(c->out_path));
    g_contacts_save_dirty = true;
    g_contacts_save_ms = millis();
    g_deferred_refresh_targets = true;
    serialmon_append("Repeater path reset to flood");
    return true;
  }

  bool discoverContactPath() {
    if (!_curr_recipient || _curr_kind != TargetKind::CONTACT) return false;
    uint32_t tag, est_timeout;
    // Path Discovery: temporarily force flood to send a telemetry request
    uint8_t req_data[9];
    req_data[0] = 0x03;  // REQ_TYPE_GET_TELEMETRY_DATA
    req_data[1] = ~(0x01);  // inverse permissions: only request base telemetry
    memset(&req_data[2], 0, 3);
    getRNG()->random(&req_data[5], 4);  // random blob for unique packet hash
    auto save = _curr_recipient->out_path_len;
    _curr_recipient->out_path_len = OUT_PATH_UNKNOWN;  // force flood
    int result = sendRequest(*_curr_recipient, req_data, sizeof(req_data), tag, est_timeout);
    _curr_recipient->out_path_len = save;  // restore
    if (result == MSG_SEND_FAILED) return false;
    _pending_discovery = tag;
    char msg[96];
    snprintf(msg, sizeof(msg), "PATH DISCOVERY: sent flood req tag=%08lX timeout=%lums",
             (unsigned long)tag, (unsigned long)est_timeout);
    serialmon_append_color(0x00FF80, msg);
    return true;
  }

  // Send to a specific contact without changing UI selection (for bridge/dashboard)
  bool sendTextToContactByPubKey(const uint8_t* pk, const char* text) {
    ContactInfo* c = lookupContactByPubKey(pk, 32);
    if (!c || !text || !text[0]) return false;
    pm_fail_existing_pending("TX PM: superseding previous pending message");
    uint32_t ts = rtc_clock.getCurrentTimeUnique();
    uint32_t ack = 0, timeout = 0;
    bool ok = sendMessage(*c, ts, 0, text, ack, timeout) != MSG_SEND_FAILED;
    if (ok) {
      String tsStr = time_string_now();
      String ck = key_for_contact(c->id);
      char bubble[512];
      snprintf(bubble, sizeof(bubble), "[%s] Me: %s", tsStr.c_str(), text);
      append_chat_to_file(ck, true, bubble, ts);

      // Allocate ring slot for ACK tracking (no UI label — bridge send)
      OutboundPM* slot = pm_alloc_slot();
      memset(slot, 0, sizeof(*slot));
      slot->active = true;
      slot->state = 'P';
      slot->msg_ts = ts;
      strncpy(slot->chat_key, ck.c_str(), sizeof(slot->chat_key) - 1);
      slot->ack_codes[0] = ack;
      slot->ack_count = 1;
      slot->retry_count = 0;
      slot->hard_timeout_ms = millis() + PM_HARD_TIMEOUT_MS;
      slot->retry_timeout_ms = millis() + pm_timeout_for_attempt(1);
      slot->recipient = c;
      strncpy(slot->retry_text, text, sizeof(slot->retry_text) - 1);
      slot->retry_text[sizeof(slot->retry_text) - 1] = '\0';
      // No status_label for headless sends, but keep the same retry cadence.

      char target_key[11];
      snprintf(target_key, sizeof(target_key), "c:%02x%02x%02x%02x",
               c->id.pub_key[0], c->id.pub_key[1], c->id.pub_key[2], c->id.pub_key[3]);

      if (g_in_chat_mode && _curr_kind == TargetKind::CONTACT && _curr_recipient &&
          memcmp(_curr_recipient->id.pub_key, pk, 32) == 0) {
        deferred_msg_push(true, bubble, nullptr, true, ts);
      }
      webdash_broadcast_message(c->name, text, true, target_key);
    }
    return ok;
  }

  // Send to a specific channel without changing UI selection (for bridge/dashboard)
  bool sendTextToChannelByIdx(int idx, const char* text) {
    ChannelDetails ch;
    if (!getChannel(idx, ch) || ch.name[0] == 0 || !text || !text[0]) return false;
    uint32_t ts = rtc_clock.getCurrentTimeUnique();
    uint8_t hash[MAX_HASH_SIZE];
    bool ok = sendGroupMessageTracked(ts, ch.channel, _prefs.node_name, text, strlen(text), hash);
    if (ok) {
      String tsStr = time_string_now();
      char bubble[512];
      snprintf(bubble, sizeof(bubble), "[%s] %s: %s", tsStr.c_str(), _prefs.node_name, text);
      append_chat_to_file(key_for_channel(idx), true, bubble, ts);

      // If this channel's chat is currently open, show the bubble live
      if (g_in_chat_mode && _curr_kind == TargetKind::CHANNEL && _curr_channel_idx == idx) {
        deferred_msg_push(true, bubble, nullptr);
      }
    }
    return ok;
  }

  void checkPmRetryTimeout() {
    OutboundPM* slot = pm_find_pending();
    if (!slot || !slot->retry_timeout_ms) return;
    if ((int32_t)(millis() - slot->retry_timeout_ms) <= 0) return;
    slot->retry_timeout_ms = 0;
    // Don't call onSendTimeout() — that would go through sendMessage() which sets
    // txt_send_timeout in the base class, causing double-fire. Do the retry directly.
    doPmRetry(slot);
  }

  void checkPmHardTimeout() {
    OutboundPM* slot = pm_find_pending();
    if (!slot || !slot->hard_timeout_ms) return;
    if ((int32_t)(millis() - slot->hard_timeout_ms) <= 0) return;

    if (slot->recipient) {
      char diag[128];
      snprintf(diag, sizeof(diag),
               "PM hard timeout, awaiting late ACK: stored path=%u, acks_tracked=%d",
               (unsigned)slot->recipient->out_path_len, (int)slot->ack_count);
      serialmon_append(diag);

      if (slot->recipient->out_path_len != OUT_PATH_UNKNOWN) {
        slot->recipient->out_path_len = OUT_PATH_UNKNOWN;
        g_contacts_save_dirty = true;
        g_contacts_save_ms = millis();
        serialmon_append("Auto-reset path to flood after hard timeout");

        if (g_in_chat_mode && _curr_recipient == slot->recipient) {
          g_chat_route_path_len = OUT_PATH_UNKNOWN;
          g_deferred_route_label_dirty = true;
        }
      }
    } else {
      serialmon_append("PM hard timeout - clearing stuck state");
    }
    pm_slot_mark_unconfirmed(slot);
    // Also evict expired entries
    pm_evict_expired();
  }

  // When a slot has been in 'U' (Probably delivered) for PM_UNCONFIRMED_MS
  // without receiving a late ACK, transition it to 'N' (Failed). The main
  // loop's existing resend-button gate (state == 'N') will then offer resend.
  void checkPmUnconfirmedExpiry() {
    uint32_t now = millis();
    for (int i = 0; i < PM_RING_SIZE; i++) {
      OutboundPM& s = g_pm_ring[i];
      if (s.active && s.state == 'U' && s.expiry_ms && (int32_t)(now - s.expiry_ms) > 0) {
        pm_slot_mark_failed(&s);
      }
    }
  }

  // Re-transmit an existing PM slot (used by the Resend button). Reuses the
  // same bubble and file entry so the user sees ONE bubble with a refreshed
  // "Sending" status — no duplicate bubble, no duplicate file line.
  bool resendSlot(OutboundPM* slot) {
    if (!slot || !slot->active || !slot->recipient || !slot->retry_text[0]) return false;
    if (slot->state != 'N' && slot->state != 'U') return false;

    // Reset slot to a fresh pending state, keep the same msg_ts so the file
    // entry is updated in place (not duplicated).
    slot->state = 'P';
    slot->retry_count = 0;
    slot->ack_count = 0;
    slot->resend_offered = false;
    // Don't set ui_dirty — apply_status_to_label doesn't render 'P' as "Sending".
    // Instead we call pm_apply_sending_label directly below.
    slot->ui_dirty = false;
    slot->file_dirty = true;
    slot->hard_timeout_ms = millis() + PM_HARD_TIMEOUT_MS;
    slot->retry_timeout_ms = millis() + pm_timeout_for_attempt(1);
    slot->expiry_ms = 0;

    uint32_t expected_ack = 0, est_timeout = 0;
    int result = sendMessage(*(slot->recipient), slot->msg_ts, 0, slot->retry_text, expected_ack, est_timeout);
    if (result == MSG_SEND_FAILED) {
      serialmon_append("Resend: sendMessage failed, marking slot unconfirmed");
      pm_slot_mark_unconfirmed(slot);
      return false;
    }

    slot->ack_codes[0] = expected_ack;
    slot->ack_count = 1;

    // Explicitly render "Sending" on the bubble's status label (same path as
    // doPmRetry for attempt 2/3). The main loop's ring processor will then
    // update to "Sending (attempt N/3)" on subsequent retries via doPmRetry,
    // and flip to 'D'/'U'/'N' via ui_dirty once the outcome is known.
    if (slot->status_label) {
      pm_apply_sending_label(slot->status_label, 1);
    }

    char logbuf[96];
    snprintf(logbuf, sizeof(logbuf), "Resend: retransmitted ts=%lu to %s",
             (unsigned long)slot->msg_ts, slot->recipient->name);
    serialmon_append(logbuf);
    return true;
  }

  // Find a slot by its status_label and resend it (used by the chat UI button).
  bool resendByStatusLabel(lv_obj_t* status_label) {
    if (!status_label) return false;
    for (int i = 0; i < PM_RING_SIZE; i++) {
      OutboundPM& s = g_pm_ring[i];
      if (s.active && s.status_label == status_label) {
        return resendSlot(&s);
      }
    }
    return false;
  }

  void bindPmStatusLabel(uint32_t msg_ts, lv_obj_t* status_label) {
    if (!status_label) return;

    OutboundPM* slot = pm_find_by_msg_ts(msg_ts);
    if (!slot) {
      serialmon_append("Deferred PM bubble: slot not found");
      apply_status_to_label(status_label, 'N');
      return;
    }

    slot->status_label = status_label;

    if (slot->state == 'P') {
      pm_apply_sending_label(status_label, slot->retry_count + 1);
    } else {
      apply_status_to_label(status_label, slot->state);
    }
  }

  void stopRepeaterConnection(const uint8_t* pub_key) { stopConnection(pub_key); }

  bool selectContactByPubKey(const uint8_t* pub_key) {
    ContactInfo* c = lookupContactByPubKey(pub_key, 32);
    if (!c) return false;
    sanitize_ascii_inplace(c->name);
    _curr_recipient = c;
    _curr_kind = TargetKind::CONTACT;
    _curr_channel_idx = -1;

    char line[96];
    snprintf(line, sizeof(line), "SELECT contact: %s", c->name);
    serialmon_append(line);

    g_chat_route_is_contact = true;
    g_chat_route_path_len = c->out_path_len;
    chat_set_header(c->name);
    load_chat_from_file(key_for_contact(c->id));
    notify_contact_clear(c->id.pub_key);
    ui_refresh_targets();
    return true;
  }

  bool selectChannelByIdx(int idx) {
    ChannelDetails ch;
    if (!getChannel(idx, ch)) return false;
    if (ch.name[0] == 0) return false;

    _curr_kind = TargetKind::CHANNEL;
    _curr_channel_idx = idx;
    _curr_recipient = nullptr;

    char tmp[64];
    StrHelper::strncpy(tmp, ch.name, sizeof(tmp));
    sanitize_ascii_inplace(tmp);

    char line[120];
    snprintf(line, sizeof(line), "SELECT channel: %s", tmp);
    serialmon_append(line);

    g_chat_route_is_contact = false;
    chat_set_header(tmp);
    load_chat_from_file(key_for_channel(idx));
    notify_channel_clear(idx);
    ui_refresh_targets();
    return true;
  }

  int joinHashtagChannel(const String& raw) {
    String input = raw;
    input.trim();
    if (!input.length()) return -1;

    String channelName;
    String psk;

    int pipe = input.indexOf('|');
    if (pipe > 0) {
      channelName = input.substring(0, pipe);
      channelName.trim();
      if (!channelName.length()) {
        serialmon_append("JOIN private: enter channel name");
        return -1;
      }
      String keyStr = input.substring(pipe + 1);
      keyStr.trim();

      uint8_t sec16[16];
      bool key_ok = false;

      if (keyStr.length() == 32) {
        for (int i = 0; i < 16; i++) {
          char hi = keyStr[i*2], lo = keyStr[i*2+1];
          auto hexval = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
          };
          int h = hexval(hi), l = hexval(lo);
          if (h < 0 || l < 0) { key_ok = false; break; }
          sec16[i] = (uint8_t)((h << 4) | l);
          key_ok = true;
        }
      } else if (keyStr.length() >= 22) {
        psk = keyStr;
        key_ok = true;
      }

      if (!key_ok && psk.length() == 0) {
        serialmon_append("JOIN private: invalid key (need 32 hex chars or base64)");
        return -1;
      }
      if (psk.length() == 0) psk = secret16_to_base64(sec16);

      sanitize_ascii_inplace((char*)channelName.c_str());

    } else {
      channelName = normalize_hashtag(input.c_str());
      if (!channelName.length()) return -1;

      uint8_t sec16[16];
      hashtag_to_secret16(channelName, sec16);
      psk = secret16_to_base64(sec16);

      char dbg[128];
      snprintf(dbg, sizeof(dbg), "JOIN hashtag %s secret16=%02x%02x%02x%02x..",
               channelName.c_str(), sec16[0], sec16[1], sec16[2], sec16[3]);
      serialmon_append(dbg);
    }

    ChannelDetails tmp;
    for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
      if (!getChannel(i, tmp)) break;
      if (tmp.name[0] == 0) break;
      if (String(tmp.name) == channelName) {
        if (i < 32 && (g_deleted_channel_mask & (1u << i))) {
          g_deleted_channel_mask &= ~(1u << i);
          saveChannelsToFS();
          char line[128];
          snprintf(line, sizeof(line), "JOIN re-added (was deleted): %s", channelName.c_str());
          serialmon_append(line);
          return i;
        }
        char line[128];
        snprintf(line, sizeof(line), "JOIN already exists: %s", channelName.c_str());
        serialmon_append(line);
        return i;
      }
    }

    ChannelDetails* added = addChannel(channelName.c_str(), psk.c_str());
    if (!added) {
      serialmon_append("JOIN channel FAILED (addChannel returned null)");
      return -1;
    }

    saveChannelsToFS();

    char line[128];
    snprintf(line, sizeof(line), "JOIN channel OK: %s", channelName.c_str());
    serialmon_append(line);

    return findChannelIdx(added->channel);
  }

  bool sendGroupMessageTracked(uint32_t timestamp, mesh::GroupChannel& channel, const char* sender_name,
                               const char* text, int text_len, uint8_t packet_hash[MAX_HASH_SIZE]) {
    uint8_t temp[5 + MAX_TEXT_LEN + 32];
    memcpy(temp, &timestamp, 4);
    temp[4] = 0;

    snprintf((char*)&temp[5], MAX_TEXT_LEN, "%s: ", sender_name);
    char* ep = strchr((char*)&temp[5], 0);
    int prefix_len = ep - (char*)&temp[5];

    if (text_len + prefix_len > MAX_TEXT_LEN) text_len = MAX_TEXT_LEN - prefix_len;
    memcpy(ep, text, text_len);
    ep[text_len] = 0;

    auto pkt = createGroupDatagram(PAYLOAD_TYPE_GRP_TXT, channel, temp, 5 + prefix_len + text_len);
    if (!pkt) return false;

    if (packet_hash) pkt->calculatePacketHash(packet_hash);
    sendFloodScoped(channel, pkt);
    return true;
  }

  void sendTextToCurrent(const char* text) {
    if (!text) return;
    while (*text == ' ') text++;
    if (!*text) return;

    uint32_t ts = rtc_clock.getCurrentTimeUnique();
    String safeText = sanitize_ascii_string(text);
    String tsStr = time_string_now();

    if (_curr_kind == TargetKind::CONTACT && _curr_recipient) {
      pm_fail_existing_pending("TX PM: superseding previous pending message");

      uint32_t expected_ack = 0, est_timeout = 0;
      int result = sendMessage(*_curr_recipient, ts, 0, safeText.c_str(), expected_ack, est_timeout);

      char namebuf[40];
      StrHelper::strncpy(namebuf, _curr_recipient->name, sizeof(namebuf));
      sanitize_ascii_inplace(namebuf);

      const char* mode_str = (result == MSG_SEND_SENT_FLOOD) ? "FLOOD" : "DIRECT";
      char line[240];
      snprintf(line, sizeof(line), "TX %s PM to %s: %s ack=%08lX timeout=%lums result=%d",
               mode_str, namebuf, safeText.c_str(),
               (unsigned long)expected_ack, (unsigned long)est_timeout, result);
      serialmon_append(line);

      if (result != MSG_SEND_FAILED) {
        // Allocate ring slot for this PM
        OutboundPM* slot = pm_alloc_slot();
        memset(slot, 0, sizeof(*slot));
        slot->active = true;
        slot->state = 'P';
        slot->msg_ts = ts;
        String ck = key_for_contact(_curr_recipient->id);
        strncpy(slot->chat_key, ck.c_str(), sizeof(slot->chat_key) - 1);
        slot->ack_codes[0] = expected_ack;
        slot->ack_count = 1;
        slot->retry_count = 0;
        slot->hard_timeout_ms = millis() + PM_HARD_TIMEOUT_MS;
        slot->retry_timeout_ms = millis() + pm_timeout_for_attempt(1);
        slot->recipient = _curr_recipient;
        strncpy(slot->retry_text, safeText.c_str(), sizeof(slot->retry_text) - 1);
        slot->retry_text[sizeof(slot->retry_text) - 1] = '\0';

        char target_key[11];
        snprintf(target_key, sizeof(target_key), "c:%02x%02x%02x%02x",
                 _curr_recipient->id.pub_key[0], _curr_recipient->id.pub_key[1],
                 _curr_recipient->id.pub_key[2], _curr_recipient->id.pub_key[3]);

        char bubble[512];
        snprintf(bubble, sizeof(bubble), "[%s] Me: %s", tsStr.c_str(), safeText.c_str());
        append_chat_to_file(ck, true, bubble, ts);
        lv_obj_t* lbl = chat_add(true, bubble, true);
        slot->status_label = lbl;
        pm_apply_sending_label(lbl, 1);

        tgbridge_forward_pm(_prefs.node_name, namebuf, safeText.c_str(), true);
        webdash_broadcast_message(namebuf, safeText.c_str(), true, target_key);
      } else {
        serialmon_append("TX PM flood send failed");
      }
      return;
    }

    if (_curr_kind == TargetKind::CHANNEL) {

      ChannelDetails ch;
      if (!getChannel(_curr_channel_idx, ch)) return;
      if (ch.name[0] == 0) return;

      char chbuf[40];
      StrHelper::strncpy(chbuf, ch.name, sizeof(chbuf));
      sanitize_ascii_inplace(chbuf);

      char line[260];
      snprintf(line, sizeof(line), "TX CH %s: %s", chbuf, safeText.c_str());
      serialmon_append(line);

      uint8_t packet_hash[MAX_HASH_SIZE] = {};
      bool ok = sendGroupMessageTracked(ts, ch.channel, _prefs.node_name, safeText.c_str(),
                                        (int)safeText.length(), packet_hash);
      if (ok) {
        char bubble[512];
        snprintf(bubble, sizeof(bubble), "[%s] %s: %s", tsStr.c_str(), _prefs.node_name, safeText.c_str());
        String key = key_for_channel(_curr_channel_idx);
        append_chat_to_file(key, true, bubble);
        tgbridge_forward_channel(chbuf, _prefs.node_name, safeText.c_str());
        char target_key[12];
        snprintf(target_key, sizeof(target_key), "h:%d", _curr_channel_idx);
        webdash_broadcast_message(chbuf, safeText.c_str(), true, target_key);

        // Show pending status, then start live discovery to count repeaters
        // that heard it.
        //
        // IMPORTANT: Mesh::onControlDataRecv() only accepts this control
        // subset when received as DIRECT + zero-hop. Flooded discover
        // responses are dropped at the Mesh layer by design, so using
        // flood here makes the receipt counter stick at 0 ("Sent").
        //
        // Therefore receipt discovery must remain zero-hop.
        char sc = 'P';
        lv_obj_t* lbl = chat_add(true, bubble, false, sc);

        clear_channel_receipt_poll();
        uint32_t rtag = sendDiscoverRepeaters(false, 700);
        if (rtag && lbl) {
          g_channel_receipt.active = true;
          g_channel_receipt.status_label = lbl;
          g_channel_receipt.chat_key = key;
          g_channel_receipt.discover_tag = rtag;
          g_channel_receipt.timeout_ms = millis() + 10000;
          g_channel_receipt.heard_count = 0;
          g_channel_receipt.seen_count = 0;
          if (lbl) lv_label_set_text(lbl, LV_SYMBOL_REFRESH " Checking...");
        } else {
          // Discovery failed — show as direct
          if (lbl) apply_receipt_count_to_label(lbl, 0, 0);
          update_last_tx_status_in_file(key, 'R', 0);
        }
      } else {
        serialmon_append("TX CH failed (sendGroupMessage returned false)");
        clear_channel_receipt_poll();
      }
      return;
    }

    serialmon_append("TX failed: no target selected");
  }

  static const Preset* presets(int& count) {
    static const Preset p[] = {
      {"EU 869.618 BW62.5 SF7", 869.618f, 62.5f, 7, 8, 22},
      {"EU 869.618 BW62.5 SF8", 869.618f, 62.5f, 8, 8, 22},
      {"EU 869.618 BW62.5 SF9", 869.618f, 62.5f, 9, 8, 22},
      {"EU 869.618 BW125 SF7",  869.618f, 125.0f, 7, 5, 22},
      {"EU 869.618 BW125 SF8",  869.618f, 125.0f, 8, 5, 22},
      {"EU 869.618 BW125 SF9",  869.618f, 125.0f, 9, 5, 22},
      {"EU 868.100 BW125 SF7",  868.100f, 125.0f, 7, 5, 20},
      {"EU 868.100 BW125 SF8",  868.100f, 125.0f, 8, 5, 20},
      {"EU 868.100 BW125 SF9",  868.100f, 125.0f, 9, 5, 20},
      {"EU 868.100 BW125 SF10", 868.100f, 125.0f, 10, 5, 20},
      {"US 915 BW125 SF7",      915.0f,   125.0f, 7, 5, 20},
      {"US 915 BW125 SF8",      915.0f,   125.0f, 8, 5, 20},
      {"US 915 BW125 SF9",      915.0f,   125.0f, 9, 5, 20},
      {"US 915 BW250 SF10",     915.0f,   250.0f, 10, 5, 20},
      {"LongRange BW62.5 SF10", 869.618f, 62.5f, 10, 8, 22},
      {"LongRange BW62.5 SF11", 869.618f, 62.5f, 11, 8, 22},
      {"LongRange BW62.5 SF12", 869.618f, 62.5f, 12, 8, 22},
    };
    count = (int)(sizeof(p) / sizeof(p[0]));
    return p;
  }

  void applyPresetByIdx(int idx, bool do_advert = true) {
    int n=0; auto p = presets(n);
    if (idx < 0 || idx >= n) return;
    _prefs.freq = p[idx].freq;
    _prefs.tx_power_dbm = p[idx].tx;
    savePrefs();
    save_preset_idx_nvs(idx);
    radio_set_params(_prefs.freq, p[idx].bw, p[idx].sf, p[idx].cr);
    radio_set_tx_power(_prefs.tx_power_dbm);

    char line[120];
    snprintf(line, sizeof(line),
             "Preset [%s] %.3f MHz BW%.1f SF%u CR%u TX%ddBm",
             p[idx].name, p[idx].freq, p[idx].bw,
             (unsigned)p[idx].sf, (unsigned)p[idx].cr, (int)p[idx].tx);
    serialmon_append(line);

    if (do_advert) sendFloodAdvert(250);
  }

  bool deleteSelectedChannel() {
    if (_curr_kind != TargetKind::CHANNEL) return false;
    if (_curr_channel_idx < 0) return false;
    if (_curr_channel_idx == _public_idx) return false;

#if defined(ESP32)
    {
      if (LittleFS.exists(CHANNELS_FILE)) {
        bool ok = true;
        File in = LittleFS.open(CHANNELS_FILE, FILE_READ);
        if (!in) ok = false;
        uint8_t count = 0;
        if (ok && in.read(&count, 1) != 1) { in.close(); ok = false; }

        File out;
        if (ok) {
          out = LittleFS.open(String(CHANNELS_FILE) + ".tmp", FILE_WRITE);
          if (!out) { in.close(); ok = false; }
        }

        if (ok) {
          uint8_t outCount = 0;
          out.write(&outCount, 1);

          ChannelDetails tmp;
          for (int i = 1; i < MAX_GROUP_CHANNELS; i++) {
            if (!getChannel(i, tmp) || tmp.name[0] == 0) break;
            if (i == _curr_channel_idx) continue;
            out.write((const uint8_t*)tmp.name, 32);
            out.write((const uint8_t*)tmp.channel.secret, 16);
            outCount++;
          }
          out.seek(0);
          out.write(&outCount, 1);
          out.close();
          in.close();
          LittleFS.remove(CHANNELS_FILE);
          LittleFS.rename(String(CHANNELS_FILE) + ".tmp", CHANNELS_FILE);
        }
      }
    }
#endif

    delete_chat_file_for_key(key_for_channel(_curr_channel_idx));
    _curr_kind = TargetKind::NONE;
    _curr_channel_idx = -1;
    _curr_recipient = nullptr;
    return true;
  }

  bool deleteContactByPubKey(const uint8_t* pub_key) {
    ContactInfo* c = lookupContactByPubKey(pub_key, 32);
    if (!c) return false;
    if (c->type == ADV_TYPE_REPEATER) return false;
    delete_chat_file_for_key(key_for_contact(c->id));
    if (_curr_recipient == c) { _curr_recipient = nullptr; _curr_kind = TargetKind::NONE; }
    ContactInfo copy = *c;
    removeContact(copy);
    saveContacts();
    return true;
  }

  bool deleteRepeaterByPubKey(const uint8_t* pub_key) {
    ContactInfo* c = lookupContactByPubKey(pub_key, 32);
    if (!c) return false;
    if (c->type != ADV_TYPE_REPEATER) return false;
    ContactInfo copy = *c;
    removeContact(copy);
    saveContacts();
    return true;
  }

  bool deleteChannelByIdx(int idx) {
    if (idx <= 0) return false;
    TargetKind   saved_kind  = _curr_kind;
    int          saved_idx   = _curr_channel_idx;
    ContactInfo* saved_rcpt  = _curr_recipient;
    _curr_kind         = TargetKind::CHANNEL;
    _curr_channel_idx  = idx;
    _curr_recipient    = nullptr;
    bool ok = deleteSelectedChannel();
    if (!ok) {
      _curr_kind        = saved_kind;
      _curr_channel_idx = saved_idx;
      _curr_recipient   = saved_rcpt;
    } else if (idx < 32) {
      g_deleted_channel_mask |= (1u << idx);
    }
    return ok;
  }

  void purgeContactsAndRepeaters() {
    purge_contacts_file_all();

    ContactInfo c;
    ContactsIterator contacts_it = startContactsIterator();
    while (contacts_it.hasNext(this, c)) {
      ContactInfo* ptr = lookupContactByPubKey(c.id.pub_key, 32);
      if (!ptr) continue;
      ptr->name[0] = 0;
      ptr->flags |= CONTACT_FLAG_HIDDEN;
      delete_chat_file_for_key(key_for_contact(ptr->id));
    }

    _curr_kind = TargetKind::NONE;
    _curr_recipient = nullptr;
    _curr_channel_idx = -1;
    g_purged_this_session = true;
    map_clear_advert_locations();
  }

  void onContactVisit(const ContactInfo&) override {}
};

static UIMesh* g_uimesh = nullptr;

// Called only on the Arduino task, never while it holds the LVGL lock.
// HTTP translation waits also use this path; callbacks remain on their
// original task and only deposit deferred UI work.
static void service_radio() {
  if (!g_mesh || !mesh_try_lock(0)) return;
  static uint32_t last_service_ms = 0;
  static uint32_t last_warning_ms = 0;
  static uint32_t last_errors = 0;
  static uint32_t last_start_errors = 0;
  const uint32_t now = millis();
  const uint32_t gap = now - last_service_ms;
  if (last_service_ms && gap > s_rx_max_service_gap_ms) s_rx_max_service_gap_ms = gap;
  s_rx_last_service_ms = now;
  const uint32_t errors = radio_driver.getPacketsRecvErrors();
  const uint32_t start_errors = radio_driver.getRxStartErrors();
  const bool delayed = last_service_ms && gap > (g_screen_awake ? 500UL : 1000UL);
  if ((delayed || errors != last_errors || start_errors != last_start_errors) &&
      (!last_warning_ms || now - last_warning_ms >= 10000UL)) {
    Serial.printf("[rx-health] service gap=%lu ms; pending RX=%u; received=%lu; read errors=%lu; RX start errors=%lu (last=%d)\n",
                  (unsigned long)gap, radio_driver.hasPendingReceiveInterrupt() ? 1u : 0u,
                  (unsigned long)radio_driver.getPacketsRecv(), (unsigned long)errors,
                  (unsigned long)start_errors, (int)radio_driver.getLastRxStartError());
    last_warning_ms = now;
    last_errors = errors;
    last_start_errors = start_errors;
  }
  last_service_ms = now;
  crash_diagnostics_mark(false, CrashStage::Radio);
  g_mesh->loop();
  crash_diagnostics_mark(false, CrashStage::Radio, 1);

  static bool previous_awake = g_screen_awake;
  static bool verify_pending = true;
  static bool report_verification = true;
  static uint32_t last_verify_ms = 0;
  static uint32_t next_attempt_ms = 0;
  if (previous_awake != g_screen_awake) {
    previous_awake = g_screen_awake;
    verify_pending = report_verification = true;
    next_attempt_ms = now;
  }
  if (now - last_verify_ms >= 30000UL) verify_pending = true;
  if (verify_pending && (int32_t)(now - next_attempt_ms) >= 0) {
    int16_t error = 0;
    crash_diagnostics_mark(false, CrashStage::Radio, 2);
    const RxGainCheck check = radio_verify_rx_gain(error);
    crash_diagnostics_mark(false, CrashStage::Radio, 3);
    next_attempt_ms = now + (check == RxGainCheck::Failed ? 10000UL : 250UL);
    if (check == RxGainCheck::Verified || check == RxGainCheck::Restored) {
      if (report_verification || check == RxGainCheck::Restored) {
        Serial.printf("[rx-health] RX gain %s; screen=%s; %s\n",
                      SX126X_RX_BOOSTED_GAIN ? "boosted" : "normal",
                      g_screen_awake ? "on" : "off",
                      check == RxGainCheck::Restored ? "restored and verified" : "verified");
      }
      verify_pending = report_verification = false;
      last_verify_ms = now;
    } else if (check == RxGainCheck::Failed) {
      Serial.printf("[rx-health] RX gain verification failed: %d; retry deferred\n", error);
    }
  }
  mesh_unlock();
}

// ============================================================
// setup()
// ============================================================

// Earliest possible debug hook — runs as a C++ ctor with low priority
// (== runs first). If this line appears on serial, static init survived.
__attribute__((constructor(101))) static void ctor_heartbeat_101() {
  ets_printf("[ctor] priority 101 ran\n");
}
__attribute__((constructor(65534))) static void ctor_heartbeat_last() {
  ets_printf("[ctor] priority 65534 (last) ran\n");
}


static const char* dbg_reset_reason_name(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_EXT:       return "EXT";
    case ESP_RST_SW:        return "SW";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_INT_WDT:   return "INT_WDT";
    case ESP_RST_TASK_WDT:  return "TASK_WDT";
    case ESP_RST_WDT:       return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "UNKNOWN";
  }
}

void setup() {
  const uint32_t boot_started_ms = millis();
  Serial.begin(115200);
  delay(50);
  Serial.println(">>> setup() entered");
  Serial.printf("[fw] CrowPanel DHE04005D direct-RGB build %s %s\n", __DATE__, __TIME__);
  const esp_reset_reason_t reset_reason = esp_reset_reason();
  Serial.printf("[boot] reset reason: %s (%d)\n",
                dbg_reset_reason_name(reset_reason), (int)reset_reason);
  crash_diagnostics_boot();
  crash_diagnostics_mark(false, CrashStage::Boot);
  Serial.flush();
  // 30-second watchdog (arduino-esp32 3.x API uses a config struct).
  {
    esp_task_wdt_config_t wdt_cfg = {};
    wdt_cfg.timeout_ms      = 30000;
    wdt_cfg.idle_core_mask  = 0;
    wdt_cfg.trigger_panic   = true;
    esp_task_wdt_reconfigure(&wdt_cfg);
  }
  esp_task_wdt_add(NULL);

  // Load the saved orientation BEFORE display_init — display_init picks
  // LVGL's hor_res/ver_res + sw_rotate based on g_landscape_mode, and the
  // settings-screen orientation toggle persists to NVS then esp_restart()s
  // so the new value is live on the next boot.
  g_landscape_mode = load_landscape_nvs();
  g_backlight_level = load_brightness_nvs();
  g_ramp_target = g_backlight_level;
  if (!display_init()) {
    Serial.println("display_init FAILED");
    while (1) delay(1000);
  }
  stc8_set_pwm_duty(STC8_PWM_LCD_BL_EN, g_backlight_level);
  Serial.printf("[boot] display ready: %lu ms from setup\n",
                (unsigned long)(millis() - boot_started_ms));

  // First frame uses built-in Latin/Greek/German fonts. The chain safely
  // skips the NULL emoji fonts; SD access must not delay this first paint.
  lv_font_greek_init();
  // Populate SCR_W/SCR_H and friends from g_landscape_mode before ui_init():
  // SquareLine screens read those globals when laying out widgets, so they
  // must be correct the first time ui_init() runs. On DHE04005D the panel is
  // a hardwired 800×480 landscape — g_landscape_mode defaults to true (see
  // keyboard_helpers.cpp).
  init_layout_constants();
  ui_init();                               // SquareLine — creates screens

  // Top-layer ownership keeps this visible across startup list rebuilds.
  lv_obj_t* boot_banner = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(boot_banner);
  lv_obj_set_width(boot_banner, lv_pct(84));
  lv_obj_set_style_max_width(boot_banner, 380, 0);
  lv_obj_set_height(boot_banner, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(boot_banner, lv_color_hex(TH_SURFACE2), 0);
  lv_obj_set_style_bg_opa(boot_banner, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(boot_banner, 1, 0);
  lv_obj_set_style_border_color(boot_banner, lv_color_hex(TH_ACCENT), 0);
  lv_obj_set_style_radius(boot_banner, 6, 0);
  lv_obj_set_style_pad_all(boot_banner, 18, 0);
  lv_obj_clear_flag(boot_banner, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_center(boot_banner);
  lv_obj_t* boot_label = lv_label_create(boot_banner);
  lv_obj_set_width(boot_label, lv_pct(100));
  lv_obj_set_style_text_font(boot_label, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(boot_label, lv_color_hex(TH_TEXT), 0);
  lv_obj_set_style_text_align(boot_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(boot_label, "Booting, please wait");

  // Post-ui_init wiring that v11's old init_display_and_ui() used to do —
  // we replaced that function with our display_init() but forgot to port
  // these steps over.
  //   * setup_keyboard(): installs our custom EN/GR/SYM layouts on every
  //     lv_keyboard instance SquareLine spawned (Keyboard1..3).
  //   * Flex on ui_chatpanel: the channel + contact bubbles created in
  //     build_homescreen_list() rely on LV_FLEX_FLOW_COLUMN to stack
  //     vertically. Without it every card ends up at (0,0) and only the
  //     last one is visible — on first boot, with just the "Public"
  //     channel, it lands under the search field and disappears.
  if (ui_Keyboard1) setup_keyboard(ui_Keyboard1);
  if (ui_Keyboard2) setup_keyboard(ui_Keyboard2);
  if (ui_Keyboard3) setup_keyboard(ui_Keyboard3);
  if (ui_chatpanel) {
    lv_obj_set_flex_flow(ui_chatpanel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ui_chatpanel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(ui_chatpanel, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(ui_chatpanel, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_scroll_snap_y(ui_chatpanel, LV_SCROLL_SNAP_NONE);
    lv_obj_clear_flag(ui_chatpanel, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_style_pad_row(ui_chatpanel, 6, 0);
    lv_obj_set_style_pad_all(ui_chatpanel, 6, 0);
    // Any divider pointer from a prior load is invalid after lv_obj_clean.
    g_pending_scroll_to_divider = nullptr;
    lv_obj_clean(ui_chatpanel);
  }
  if (ui_Keyboard1) lv_obj_add_flag(ui_Keyboard1, LV_OBJ_FLAG_HIDDEN);
  if (ui_Keyboard2) lv_obj_add_flag(ui_Keyboard2, LV_OBJ_FLAG_HIDDEN);
  if (ui_Keyboard3) lv_obj_add_flag(ui_Keyboard3, LV_OBJ_FLAG_HIDDEN);
  if (ui_textsendtype) {
    lv_obj_set_y(ui_textsendtype, TEXTSEND_Y_DEFAULT);
    lv_obj_add_flag(ui_textsendtype, LV_OBJ_FLAG_HIDDEN);
  }

  // ── Settings-screen button labels + dynamically-created buttons ──
  // v11's init_display_and_ui() did this block after ui_init(). We
  // replaced that function with display_init() and dropped the block —
  // which is why every action button on the settings screen rendered
  // blank. Port the whole thing verbatim.
  {
    lv_obj_t* green_btns[] = {
      ui_fadvertbutton, ui_zeroadvertbutton, ui_presetpickbutton,
      ui_notificationstoggle,
      ui_repeatersbutton, ui_autocontacttoggle, ui_autorepeatertoggle,
      ui_repeateradvertbutton, ui_neighboursbutton, ui_rebootbutton,
      ui_repeaterloginbutton, ui_mutebutton, ui_statusbutton
    };
    for (auto b : green_btns) {
      if (b) lv_obj_set_style_bg_color(b, lv_color_hex(g_theme->btn_active),
                                        LV_PART_MAIN | LV_STATE_DEFAULT);
    }
  }
  if (ui_purgedatabutton) {
    lv_obj_set_style_bg_color(ui_purgedatabutton, lv_color_hex(g_theme->btn_danger),
                              LV_PART_MAIN | LV_STATE_DEFAULT);
  }
  if (ui_Label9) {
    lv_label_set_text(ui_Label9, LV_SYMBOL_UPLOAD "\nFlood-Advert");
    lv_obj_set_style_text_align(ui_Label9, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(ui_Label9);
  }
  if (ui_Label8) {
    lv_label_set_text(ui_Label8, LV_SYMBOL_REFRESH "\n0-Advert");
    lv_obj_set_style_text_align(ui_Label8, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(ui_Label8);
  }
  if (ui_Label5)  lv_label_set_text(ui_Label5,  LV_SYMBOL_SETTINGS " Select");
  if (ui_Label11) lv_label_set_text(ui_Label11, LV_SYMBOL_CALL     "\nAuto Contacts");
  if (ui_Label20) lv_label_set_text(ui_Label20, LV_SYMBOL_BELL     " Notify");
  btn_lbl(ui_purgedatabutton,    LV_SYMBOL_TRASH    "\nPurge Data");
  btn_lbl(ui_rebootsettingsbutton, LV_SYMBOL_POWER  "\nReboot Device");
  btn_lbl(ui_repeatersbutton,    LV_SYMBOL_WIFI     "\nRepeaters");
  btn_lbl(ui_autorepeatertoggle, LV_SYMBOL_WIFI     "\nAuto Repeaters");
  btn_lbl(ui_autocontacttoggle,  LV_SYMBOL_CALL     "\nAuto Contacts");
  btn_lbl(ui_statusbutton,       LV_SYMBOL_REFRESH  "\nStatus");

  // Speaker button — created next to the Notifications toggle
  if (ui_notificationstoggle) {
    lv_obj_t* parent = lv_obj_get_parent(ui_notificationstoggle);
    if (parent) {
      g_speaker_btn = lv_btn_create(parent);
      lv_obj_set_size(g_speaker_btn, BTN_HALF_W, 50);
      lv_obj_set_style_radius(g_speaker_btn, 4, 0);
      lv_obj_set_style_border_opa(g_speaker_btn, LV_OPA_TRANSP, 0);
      lv_obj_set_style_shadow_opa(g_speaker_btn, LV_OPA_TRANSP, 0);
      lv_obj_set_style_pad_all(g_speaker_btn, 4, 0);
      lv_obj_t* sl = lv_label_create(g_speaker_btn);
      lv_obj_set_style_text_font(sl, &lv_font_montserrat_14, 0);
      lv_obj_set_style_text_align(sl, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_set_style_text_color(sl, lv_color_white(), 0);
      lv_obj_center(sl);
      lv_obj_add_event_cb(g_speaker_btn, cb_speaker_toggle, LV_EVENT_CLICKED, nullptr);
      ui_apply_speaker_btn_state();
    }
  }

  // Orientation toggle — swaps LVGL between landscape (800×480 native) and
  // portrait (480×800 via sw_rotate=1, LV_DISP_ROT_90). cb_orientation_toggle
  // persists the new value to NVS and calls esp_restart() so display_init()
  // picks up the change on the next boot.
  if (ui_orientationtoggle) {
    lv_obj_set_style_bg_color(ui_orientationtoggle, lv_color_hex(g_theme->btn_active), 0);
    lv_obj_add_event_cb(ui_orientationtoggle, cb_orientation_toggle, LV_EVENT_CLICKED, nullptr);
    ui_apply_orientation_btn_state();
  }

  // Repeater Discovery + Floor Noise buttons are reparented into the
  // top-of-settings QUICK ACTIONS flex-wrap container (ui_settings_ser_btncol)
  // alongside ui_autocontacttoggle + ui_autorepeatertoggle, so the four action
  // buttons share the same 4-across (landscape) / 2×2 (portrait) grid.
  // Packet Forward was removed per user request — its logic (cb_packet_forward_toggle
  // / ui_apply_packet_forward_state) remains available in settings_cb.cpp if
  // it's ever brought back.
  if (ui_settings_ser_btncol) {
    lv_coord_t bw = g_landscape_mode ? lv_pct(23) : lv_pct(48);
    int16_t    bh = 50;

    g_discover_repeaters_btn = lv_btn_create(ui_settings_ser_btncol);
    lv_obj_set_size(g_discover_repeaters_btn, bw, bh);
    lv_obj_set_style_radius(g_discover_repeaters_btn, 4, 0);
    lv_obj_set_style_bg_color(g_discover_repeaters_btn, lv_color_hex(g_theme->btn_active), 0);
    lv_obj_set_style_border_opa(g_discover_repeaters_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_opa(g_discover_repeaters_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(g_discover_repeaters_btn, 4, 0);
    { lv_obj_t* dl = lv_label_create(g_discover_repeaters_btn);
      lv_label_set_text(dl, LV_SYMBOL_GPS "\nRpt Discovery");
      lv_obj_set_style_text_font(dl, &lv_font_montserrat_14, 0);
      lv_obj_set_style_text_align(dl, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_set_style_text_color(dl, lv_color_white(), 0);
      lv_obj_center(dl); }
    lv_obj_add_event_cb(g_discover_repeaters_btn, cb_discover_repeaters, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* noise_btn = lv_btn_create(ui_settings_ser_btncol);
    lv_obj_set_size(noise_btn, bw, bh);
    lv_obj_set_style_radius(noise_btn, 4, 0);
    lv_obj_set_style_bg_color(noise_btn, lv_color_hex(g_theme->btn_active), 0);
    lv_obj_set_style_border_opa(noise_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_opa(noise_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(noise_btn, 4, 0);
    { lv_obj_t* nl = lv_label_create(noise_btn);
      lv_label_set_text(nl, LV_SYMBOL_AUDIO "\nFloor Noise");
      lv_obj_set_style_text_font(nl, &lv_font_montserrat_14, 0);
      lv_obj_set_style_text_align(nl, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_set_style_text_color(nl, lv_color_white(), 0);
      lv_obj_center(nl); }
    lv_obj_add_event_cb(noise_btn, cb_floor_noise, LV_EVENT_CLICKED, nullptr);
  }

  // First gauge sample before the startup frame; no LVGL task/mutex is active yet.
  battery_poll();
  update_batterylabel();

  // Paint the first frame NOW. Everything after this (board.begin, radio
  // init, MeshCore, WiFi.mode, WiFi.begin…) can block for several seconds,
  // and without a tick of lv_timer_handler() the RGB framebuffer stays at
  // its uninitialised contents until loop() runs — which is why the panel
  // appeared dark until WiFi associated. A handful of iterations gives
  // the panel at least one vsync flip with the home screen painted.
  for (int i = 0; i < 8; i++) { lv_timer_handler(); delay(5); }
  Serial.printf("[boot] first UI frame submitted: %lu ms from setup\n",
                (unsigned long)(millis() - boot_started_ms));

  uint32_t stage_started_ms = millis();
  if (ui_repeaterscreen)
    lv_obj_add_event_cb(ui_repeaterscreen, cb_repeater_screen_loaded,
                        LV_EVENT_SCREEN_LOADED, nullptr);
  if (ui_chatpanel) g_chatpanel_orig_y = lv_obj_get_y(ui_chatpanel);
  // DHE04005D has no battery-backed RTC — NTP is the sole time source
  // (once WiFi is up). No RTC init/probe step on this variant.
  speaker_init();                          // I2S audio out (LRCK=21, BCLK=22, SDOUT=23)
  load_ui_prefs_nvs();
  if (ui_screentimeout) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)g_screen_timeout_s);
    lv_textarea_set_text(ui_screentimeout, buf);
  }

  board.begin();
  stage_started_ms = millis();
  if (!radio_init()) {
    serialmon_append("Radio init failed");
    while (1) delay(1000);
  }

  StdRNG& rng = fast_rng;
  rng.begin(radio_get_rng_seed());
  chat_persistence_init();

#if defined(NRF52_PLATFORM)
  InternalFS.begin();
  g_uimesh = new UIMesh(radio_driver, rng, rtc_clock, tables);
  g_mesh = g_uimesh;
  g_uimesh->begin(InternalFS);
#elif defined(RP2040_PLATFORM)
  LittleFS.begin();
  g_uimesh = new UIMesh(radio_driver, rng, rtc_clock, tables);
  g_mesh = g_uimesh;
  g_uimesh->begin(LittleFS);
#elif defined(ESP32)
  // LittleFS is already mounted with the correct partition label
  // ("mcdata") by radio_init() in variants/crowpanel_dhe04005d/target.cpp.
  // Calling LittleFS.begin() here with no label triggers a stray probe
  // for a "spiffs" partition that doesn't exist in our table, producing
  // the misleading `partition "spiffs" could not be found` error log.
  g_uimesh = new UIMesh(radio_driver, rng, rtc_clock, tables);
  g_mesh = g_uimesh;
  if (!g_mesh_mutex) {
    g_mesh_mutex = xSemaphoreCreateMutex();
    if (!g_mesh_mutex) Serial.println("[mesh] WARN: failed to create mesh mutex");
  }
  g_uimesh->begin(LittleFS);
#else
  #error "need to define filesystem"
#endif

  Serial.printf("[boot] radio and mesh storage: %lu ms\n",
                (unsigned long)(millis() - stage_started_ms));

  {
    double saved_lat = 0.0;
    double saved_lon = 0.0;
    if (load_fixed_position_nvs(&saved_lat, &saved_lon)) {
      g_fixed_position_valid = true;
      g_fixed_position_lat = saved_lat;
      g_fixed_position_lon = saved_lon;
      g_uimesh->setNodePosition(saved_lat, saved_lon);
    } else {
      g_fixed_position_valid = false;
      g_fixed_position_lat = 0.0;
      g_fixed_position_lon = 0.0;
    }
  }

  // Keep chat/CLI text framing in legacy 1-byte mode for interoperability.
  g_uimesh->setTextHeaderBytes(1);

  // The optional external RTC was loaded during board initialization.

  radio_set_params(g_uimesh->getFreqPref(), (float)LORA_BW, (uint8_t)LORA_SF, (uint8_t)LORA_CR);
  radio_set_tx_power(g_uimesh->getTxPowerPref());

  if (ui_devicenamelabel) lv_label_set_text(ui_devicenamelabel, g_uimesh->getNodeName());

  ui_refresh_targets();
  // Saved names stay UTF-8 in the mesh/list model. Paint chats before optional
  // SD assets and network services; emoji glyphs will join the font chain later.
  for (int i = 0; i < 8; i++) { lv_timer_handler(); delay(5); }
  Serial.printf("[boot] chat list frame submitted: %lu ms from setup\n",
                (unsigned long)(millis() - boot_started_ms));
  ui_populate_presets();
  ui_populate_timezone_dropdown();
  reg(ui_timezonedropdown, cb_timezone, LV_EVENT_VALUE_CHANGED);
  reg(ui_headerbytesdropdown, cb_header_bytes_changed, LV_EVENT_VALUE_CHANGED);
  if (ui_headerbytesdropdown) {
    uint8_t phb = g_uimesh->getPathHashBytes();
    uint16_t sel = (phb <= 1) ? 0 : (phb == 2) ? 1 : 2;
    lv_dropdown_set_selected(ui_headerbytesdropdown, sel);
  }

  {
    int saved_idx = load_preset_idx_nvs();
    if (saved_idx >= 0)
      g_uimesh->applyPresetByIdx(saved_idx, false);
  }

  if (ui_textsendtype) {
    lv_textarea_set_max_length(ui_textsendtype, TEXTSEND_MAX_CHARS);
  }

  reg(ui_textsendtype,    cb_textsend_ready, LV_EVENT_READY);
  reg(ui_textsendtype,    cb_textsend_changed, LV_EVENT_VALUE_CHANGED);
  // Defensive: on some LVGL keyboard layouts/versions the READY event
  // is emitted by the keyboard widget but not forwarded to the bound
  // textarea. Wire both so "enter/check" always sends.
  reg(ui_Keyboard1,       cb_textsend_ready, LV_EVENT_READY);
  reg(ui_renamebox,       cb_rename_ready,   LV_EVENT_READY);
  reg(ui_renamebox,       cb_rename_ready,   LV_EVENT_CANCEL);
  reg(ui_screentimeout,   cb_timeout_ready,  LV_EVENT_READY);
  reg(ui_hashtagchannel,  cb_hashtag_ready,  LV_EVENT_READY);
  reg(ui_privatechannelname, cb_private_channel_ready, LV_EVENT_READY);
  reg(ui_privatechannelhex,  cb_private_channel_ready, LV_EVENT_READY);
  reg(ui_searchfield,     cb_searchfield_ready, LV_EVENT_READY);

  reg(ui_textsendtype,    cb_textsend_focused, LV_EVENT_FOCUSED);
  reg(ui_textsendtype,    cb_textsend_focused, LV_EVENT_CLICKED);
  // LV_EVENT_CLICKED (not FOCUSED) so a press-and-scroll gesture that
  // happens to start on a text field doesn't bring the keyboard up.
  // CLICKED fires "on release if not scrolled" (LVGL 8.3 lv_event.h:42);
  // FOCUSED fires on press regardless of subsequent scroll → grabbed kb.
  reg(ui_renamebox,       cb_rename_focused,   LV_EVENT_CLICKED);
  reg(ui_screentimeout,   cb_timeout_focused,  LV_EVENT_CLICKED);
  reg(ui_txpowerslider,   cb_txpower_focused,  LV_EVENT_CLICKED);
  reg(ui_hashtagchannel,  cb_hashtag_focused,  LV_EVENT_CLICKED);
  reg(ui_privatechannelname, cb_private_channel_name_focused, LV_EVENT_CLICKED);
  reg(ui_privatechannelhex,  cb_private_channel_hex_focused,  LV_EVENT_CLICKED);
  reg(ui_searchfield,     cb_searchfield_focused, LV_EVENT_FOCUSED);
  reg(ui_searchfield,     cb_searchfield_focused, LV_EVENT_CLICKED);

  reg(ui_textsendtype,    cb_textsend_defocused,     LV_EVENT_DEFOCUSED);
  reg(ui_renamebox,       cb_rename_defocused,       LV_EVENT_DEFOCUSED);
  reg(ui_screentimeout,   cb_timeout_defocused,      LV_EVENT_DEFOCUSED);
  reg(ui_txpowerslider,   cb_txpower_defocused,      LV_EVENT_DEFOCUSED);
  reg(ui_hashtagchannel,  cb_hashtag_defocused,      LV_EVENT_DEFOCUSED);
  reg(ui_privatechannelname, cb_private_channel_name_defocused, LV_EVENT_DEFOCUSED);
  reg(ui_privatechannelhex,  cb_private_channel_hex_defocused,  LV_EVENT_DEFOCUSED);
  reg(ui_searchfield,     cb_searchfield_defocused,  LV_EVENT_DEFOCUSED);

  reg(ui_searchfield,     cb_searchfield_changed, LV_EVENT_VALUE_CHANGED);

  reg(ui_backbutton, cb_back_button);

  if (ui_mutebutton) {
    lv_obj_add_flag(ui_mutebutton, LV_OBJ_FLAG_HIDDEN);
    reg(ui_mutebutton, cb_mute_toggle);
    ui_apply_mute_button_state();
  }

  if (ui_chatpanel) {
    lv_obj_add_event_cb(ui_chatpanel, cb_chatpanel_scroll, LV_EVENT_SCROLL, nullptr);
  }

  reg(ui_brightnessslider, cb_brightness, LV_EVENT_VALUE_CHANGED);
  reg(ui_brightnessslider, cb_brightness, LV_EVENT_RELEASED);
  if (ui_brightnessslider)
    lv_slider_set_value(ui_brightnessslider,
        ScreenPower::sliderFromBrightness(g_backlight_level,
            lv_slider_get_max_value(ui_brightnessslider)), LV_ANIM_OFF);

  // Volume slider: live-update g_speaker_volume on every drag delta
  // (VALUE_CHANGED) but only persist to NVS and preview-beep on
  // RELEASED, so dragging the knob doesn't thrash flash or spam beeps.
  if (ui_volumeslider) {
    lv_slider_set_value(ui_volumeslider, g_speaker_volume, LV_ANIM_OFF);
    lv_obj_add_event_cb(ui_volumeslider, cb_volume, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(ui_volumeslider, cb_volume, LV_EVENT_RELEASED,      nullptr);
  }

  // Notification-sound dropdown: populate from the bank defined in
  // utils.cpp, select the currently-saved option, and wire Test / Save.
  if (ui_sounddropdown) {
    lv_dropdown_set_options(ui_sounddropdown, notification_sound_names_newline_list());
    uint16_t sel = g_notification_sound_idx;
    if ((int)sel >= notification_sound_count()) sel = 0;
    lv_dropdown_set_selected(ui_sounddropdown, sel);
  }
  reg(ui_soundtestbutton, cb_sound_test);
  reg(ui_soundsavebutton, cb_sound_save);

  // Restore saved TX power from NVS
  {
    Preferences p;
    p.begin("ui", true);
    int8_t saved_tx = p.getChar("tx_power", 0);
    p.end();
    if (saved_tx >= 1 && saved_tx <= 22) {
      radio_set_tx_power(saved_tx);
      mesh_set_tx_power_pref(saved_tx);
      char line[40];
      snprintf(line, sizeof(line), "TX power restored: %d dBm", (int)saved_tx);
      serialmon_append(line);
    }
  }
  if (ui_txpowerslider && g_uimesh) {
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", (int)g_uimesh->getTxPowerPref());
    lv_textarea_set_text(ui_txpowerslider, buf);
    reg(ui_txpowerslider, cb_txpower_ready, LV_EVENT_READY);
  }

  reg(ui_presetpickbutton,  cb_preset);
  reg(ui_presetsdropdown,   cb_preset, LV_EVENT_VALUE_CHANGED);
  reg(ui_fadvertbutton,     cb_fadvert);
  reg(ui_zeroadvertbutton,  cb_zeroadvert);
  reg(ui_positionadverttoggle, cb_position_advert_toggle, LV_EVENT_VALUE_CHANGED);

  reg(ui_purgedatabutton, cb_purge_data);
  reg(ui_rebootsettingsbutton, cb_reboot_device);

  reg(ui_notificationstoggle, cb_notifications_toggle);
  ui_apply_notifications_state();
  // Refresh the Sound-button label now that load_ui_prefs_nvs() has run
  // earlier in setup(). The first ui_apply_speaker_btn_state() call (right
  // after the button was created, before load_ui_prefs_nvs) showed
  // whatever g_speaker_enabled's default init was (true) — not the
  // persisted value. Without this second call, the button could read
  // "Sound ON" while the underlying flag was actually false → visible as
  // "toggled the button to OFF once, survived a reboot, button is back
  // to saying ON but nothing plays".
  ui_apply_speaker_btn_state();

  reg(ui_autocontacttoggle,  cb_auto_contact_toggle);
  reg(ui_autorepeatertoggle, cb_auto_repeater_toggle);

  // Translation wiring is in features_cb.cpp (Web Apps screen)

  // Optional SD and emoji assets are loaded last, after the chat list is visible.

  // WiFi subsystem init — non-blocking. wifi_init() wires SDIO pins, loads
  // saved NVS credentials (bootstrapping from compile-time WIFI_SSID/_PASS
  // on first boot if NVS is empty), and kicks off STA association. The
  // actual connect completes asynchronously and is picked up by wifi_loop()
  // which also drives NTP sync + reconnect backoff.
  Serial.printf("[heap] before wifi: internal=%u free=%u minfree=%u | psram=%u free=%u\n",
                (unsigned)heap_caps_get_total_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  wifi_init();

  // Features & Bridges — all stubbed for now; will be re-enabled as each
  // .cpp gets ported to work with the P4's ESP-Hosted WiFi.
  ota_init();
  webdash_init();
  tgbridge_init();
  translate_init();


#if ENABLE_ADVERT_ON_BOOT == 1
  g_uimesh->sendFloodAdvert(1200);
#endif

  serialmon_append("UI+MeshCore ready");
  load_notifications_nvs();
  resolve_pending_on_boot();
  ui_apply_auto_contact_state();
  ui_apply_auto_repeater_state();
  ui_apply_position_advert_state();

  ui_refresh_targets();

  // Last boot stage: no input device or LVGL service task can use a half-built
  // font chain. Atlas data is initialized once; stored names/messages are intact.
  stage_started_ms = millis();
  sd_init();
  Serial.printf("[boot] SD mount: %lu ms\n", (unsigned long)(millis() - stage_started_ms));
  esp_task_wdt_reset();
  stage_started_ms = millis();
  emoji_atlas_init();
  emoji_font_init();
  lv_font_greek_init();
  lv_obj_report_style_change(nullptr);
  g_homescreen_list_is_current = false;
  ui_refresh_targets();
  lv_obj_invalidate(lv_scr_act());
  for (int i = 0; i < 8; i++) { lv_timer_handler(); delay(5); }
  Serial.printf("[boot] emoji assets and chat refresh: %lu ms\n",
                (unsigned long)(millis() - stage_started_ms));
  esp_task_wdt_reset();
  display_touch_attach();                  // Enable interaction only after fonts are ready.

  // Initialization is complete; deletion invalidates the covered area for
  // the first serviced frame. No LVGL task is running during this deletion.
  lv_obj_del(boot_banner);

  // All setup-time widget creation is done — safe to start the LVGL service
  // task now. From this point on, anything touching LVGL from outside the
  // task must use lvgl_lock()/lvgl_unlock().
  display_start_lvgl_task();
  Serial.printf("[boot] UI service ready: %lu ms from setup\n",
                (unsigned long)(millis() - boot_started_ms));
}

// ============================================================
// loop()
// ============================================================

static void cpu_light_sleep_poll() {
  if (!ScreenPower::kCpuLightSleep) return;
  static bool disabled = false;
  static bool dark_session = false;
  static bool blocked_reported = false;
  static bool domains_configured = false;
  static uint32_t sleeps = 0, radio_wakes = 0, touch_wakes = 0, rejects = 0;
  static uint64_t elapsed_us = 0;
  if (g_screen_awake) {
    if (dark_session && (sleeps || rejects)) {
      Serial.printf("[power] CPU light sleep: intervals=%lu elapsed=%llu ms radio_wakes=%lu touch_wakes=%lu rejects=%lu\n",
          (unsigned long)sleeps, (unsigned long long)(elapsed_us / 1000),
          (unsigned long)radio_wakes, (unsigned long)touch_wakes, (unsigned long)rejects);
    }
    dark_session = false;
    return;
  }
  if (!dark_session) {
    dark_session = true;
    blocked_reported = false;
    sleeps = radio_wakes = touch_wakes = rejects = 0;
    elapsed_us = 0;
  }
  if (disabled) return;
  if (!wifi_cpu_sleep_safe()) {
    if (!blocked_reported) {
      Serial.println("[power] CPU light sleep blocked: hosted WiFi used; reboot with WiFi off");
      blocked_reported = true;
    }
    return;
  }

  // Match UI callbacks' LVGL -> mesh lock order. Audio holds neither lock.
  if (!lvgl_lock(0)) return;
  if (!mesh_try_lock(0)) { lvgl_unlock(); return; }
  if (!ScreenPower::cpuSleepReady(display_cpu_sleep_ready(), wifi_cpu_sleep_safe(),
          radio_driver.isReceiveIdleForCpuSleep(),
          !g_uimesh || g_uimesh->hasPendingRadioPackets())
      || !speaker_try_idle_lock()) {
    mesh_unlock();
    lvgl_unlock();
    return;
  }

  esp_err_t err = ESP_OK;
  if (!domains_configured) {
    // Retain SPI/I2C/PPA state and external RAM/flash supplies. Only clock-gate.
    err = esp_sleep_pd_config(ESP_PD_DOMAIN_TOP, ESP_PD_OPTION_ON);
    if (err == ESP_OK) err = esp_sleep_pd_config(ESP_PD_DOMAIN_CNNT, ESP_PD_OPTION_ON);
    if (err == ESP_OK) err = esp_sleep_pd_config(ESP_PD_DOMAIN_VDDSDIO, ESP_PD_OPTION_ON);
    domains_configured = err == ESP_OK;
  }
  constexpr gpio_num_t dio = static_cast<gpio_num_t>(P_LORA_DIO_1);
  constexpr gpio_num_t touch = static_cast<gpio_num_t>(Touch_GPIO_INT);
  const int touch_level = ScreenPower::kTouchIrqSleep ? gt911_interrupt_wake_level() : -1;
  bool irq_masked = false;
  bool touch_armed = false;
  bool touch_setup_attempted = false;
  bool touch_pending = false;
  bool radio_pending = false;
  bool attempted = false;
  uint64_t duration_us = 0;
  esp_sleep_wakeup_cause_t cause = ESP_SLEEP_WAKEUP_UNDEFINED;
  if (err == ESP_OK) {
    err = gpio_intr_disable(dio);
    irq_masked = err == ESP_OK;
  }
  // GPIO wake shares intr_type with the CPU ISR. Mask it before enabling level
  // wake to avoid an interrupt storm while SX1262 keeps RxDone asserted.
  if (err == ESP_OK) err = gpio_wakeup_enable(dio, GPIO_INTR_HIGH_LEVEL);
  if (err == ESP_OK && touch_level >= 0) {
    touch_setup_attempted = true;
    esp_err_t touch_err = gpio_intr_disable(touch);
    if (touch_err == ESP_OK)
      touch_err = gpio_wakeup_enable(touch,
          touch_level ? GPIO_INTR_HIGH_LEVEL : GPIO_INTR_LOW_LEVEL);
    touch_armed = touch_err == ESP_OK;
    if (!touch_armed) {
      // Do not lose the known-good radio/polling sleep path on a touch GPIO
      // setup error. Disable only this optimization for the rest of the boot.
      gt911_disable_interrupt_wake();
      Serial.printf("[power] touch GPIO wake unavailable (%d); keeping 80 ms polling\n", (int)touch_err);
    }
  }
  const uint32_t sleep_ms = ScreenPower::cpuSleepPeriodMs(touch_armed);
  if (err == ESP_OK) err = esp_sleep_enable_gpio_wakeup();
  if (err == ESP_OK) err = esp_sleep_enable_timer_wakeup(sleep_ms * 1000ULL);
  touch_pending = touch_armed && gpio_get_level(touch) == touch_level;
  if (err == ESP_OK && radio_driver.isReceiveIdleForCpuSleep()
      && ScreenPower::wakeInputsQuiet(gpio_get_level(dio) != 0,
          touch_armed, touch_pending, g_screen_awake)) {
    attempted = true;
    const int64_t start_us = esp_timer_get_time();
    err = esp_light_sleep_start();
    duration_us = esp_timer_get_time() - start_us;
    cause = esp_sleep_get_wakeup_cause();
  }
  radio_pending = gpio_get_level(dio) != 0;
  touch_pending = touch_pending || (touch_armed && gpio_get_level(touch) == touch_level);

  esp_err_t restore_err = ESP_OK;
  if (touch_setup_attempted) {
    restore_err = gpio_wakeup_disable(touch);
    const esp_err_t touch_restore_err = gpio_set_intr_type(touch, GPIO_INTR_DISABLE);
    if (restore_err == ESP_OK) restore_err = touch_restore_err;
  }
  if (irq_masked) {
    const esp_err_t radio_restore_err = gpio_wakeup_disable(dio);
    if (restore_err == ESP_OK) restore_err = radio_restore_err;
    const esp_err_t edge_err = gpio_set_intr_type(dio, GPIO_INTR_POSEDGE);
    const esp_err_t enable_err = gpio_intr_enable(dio);
    if (restore_err == ESP_OK) restore_err = edge_err;
    if (restore_err == ESP_OK) restore_err = enable_err;
    // A rising edge during the masked window is otherwise lost. Leave the
    // SX1262 FIFO/IRQ untouched; recvRaw() handles it on the next mesh loop.
    if (gpio_get_level(dio)) radio_driver.latchReceiveInterruptAfterCpuSleep();
  }
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
  if (ScreenPower::touchRequestedWake(touch_armed, touch_pending,
          attempted && cause == ESP_SLEEP_WAKEUP_GPIO, radio_pending)) {
    // Preserve a short, verified touch IRQ even if its coordinate read is late.
    g_swallow_touch = true;
    g_touch_was_press = false;
    note_touch_activity();
  }
  speaker_idle_unlock();
  mesh_unlock();
  lvgl_unlock();
  if (attempted || touch_pending) display_notify_service();

  if (restore_err != ESP_OK || (err != ESP_OK
      && (!attempted || err != ESP_ERR_SLEEP_REJECT))) {
    disabled = true;
    Serial.printf("[power] CPU light sleep disabled: sleep=%d restore=%d\n", (int)err, (int)restore_err);
  } else if (attempted && err == ESP_OK) {
    elapsed_us += duration_us;
    if (cause == ESP_SLEEP_WAKEUP_GPIO) {
      if (radio_pending) ++radio_wakes;
      // A GT911 edge-mode pulse can end before this sample; its unread touch
      // buffer remains pending. The only other GPIO wake source here is LoRa.
      if (touch_armed && (touch_pending || !radio_pending)) ++touch_wakes;
    }
    if (++sleeps == 1)
      Serial.printf("[power] CPU light sleep active: timer=%lu ms; LoRa DIO1 wake; touch=%s; RX stays on\n",
          (unsigned long)sleep_ms, touch_armed ? "GPIO wake" : "polling fallback");
  } else if (attempted) {
    ++rejects;
  }
}

void loop() {
  crash_diagnostics_poll_serial();
  crash_diagnostics_mark(false, CrashStage::Input);
  crash_diagnostics_sample(false);
  esp_task_wdt_reset();

  // Drain a completed RX before battery/I2C, history widgets or networking.
  service_radio();

  // Poll the onboard STC8 battery gauge OFF the LVGL lock (self-throttled to
  // 5 s). The status-bar % is applied later under the lock via the deferred
  // g_deferred_battery_dirty path.
  battery_poll();

  if (g_confirm_action != ConfirmAction::NONE) {
    if ((int32_t)(millis() - g_confirm_deadline_ms) > 0) {
      if (g_confirm_action == ConfirmAction::PURGE_DATA) purge_btn_restore_position();
      confirm_clear();
    }
  }

  {
    static uint32_t last_time_update_ms = 0;
    static bool     timelabel_primed   = false;
    uint32_t now_ms = millis();
    // Force an update on the very first loop iteration so the "--:--"
    // placeholder gets replaced immediately when the RTC has loaded a
    // valid time at boot (otherwise we wait up to 60 s for the next
    // tick — or until NTP overwrites and sets the dirty flag).
    //
    // update_timelabel() calls lv_label_set_text() which mutates LVGL
    // state. lvgl_task may be mid-render simultaneously, so the
    // mutation must happen under the LVGL mutex — otherwise we hit
    // `_lv_inv_area: detected modifying dirty areas in render` (and
    // worse, can corrupt the dirty-area list). Wrap with lvgl_lock.
    if (g_screen_awake && (!timelabel_primed || (now_ms - last_time_update_ms >= 60000UL))) {
      // Recompute DST on the first paint: load_ui_prefs_nvs() ran during
      // setup() before the RTC had loaded a valid epoch from the chip,
      // so the offset is currently locked at the standard-time value
      // (winter offset). Now that getCurrentTime() returns real UTC, we
      // can pick the correct DST-aware offset before painting.
      if (!timelabel_primed) tz_update_offset_now();
      if (lvgl_lock(50)) {
        timelabel_primed    = true;
        last_time_update_ms = now_ms;
        update_timelabel();
        lvgl_unlock();
      }
      // If we couldn't grab the lock within 50 ms we'll retry on the
      // next loop iteration — no big deal, the time only ticks once per
      // 60 s normally.
    }
  }

  uint32_t now = millis();
  const bool ui_interacting = ((now - g_last_touch_ms) < 450);
  uint32_t timeout_ms = g_screen_timeout_s * 1000UL;

  if (g_screen_awake) {
    if (ScreenPower::screenTimeoutDue(now, g_last_touch_ms, timeout_ms,
                                     display_backlight_ready())) {
      screen_sleep();
    }
  } else {
    // P4 port: v11 polled LovyanGFX's touch to wake the screen. Our GT911
    // driver emits through LVGL's indev, which is polled continuously; any
    // press updates g_last_touch_ms via the indev read path. Nothing to do
    // here.
  }

  // ---- Soft wake-up ramp tick ----
  if (!ScreenPower::kDisplayTaskBacklight && g_screen_awake && display_backlight_ready() && g_ramp_current > 0 &&
      (int32_t)(now - g_ramp_next_ms) >= 0) {
    const uint8_t next_level = ScreenPower::nextRampLevel(g_ramp_current, g_ramp_target);
    if (i2c_cmd(next_level)) {
      g_ramp_current = next_level;
      g_ramp_next_ms = now + RAMP_STEP_MS;
      if (g_ramp_current >= g_ramp_target) g_ramp_current = 0;
    } else {
      // Do not finish the ramp on an I2C failure and leave PWM stuck at zero.
      g_ramp_next_ms = now + 1000;
    }
  }

  bool pm_ui_dirty = false;
  bool pm_resend_offer = false;
  bool pm_file_dirty = false;
  for (int i = 0; i < PM_RING_SIZE; i++) {
    const OutboundPM& s = g_pm_ring[i];
    if (!s.active) continue;
    if (s.ui_dirty && s.status_label) pm_ui_dirty = true;
    if (s.file_dirty && s.chat_key[0]) pm_file_dirty = true;
    if (s.state == 'N' && s.status_label && !s.resend_offered &&
        s.recipient && s.retry_text[0]) {
      pm_resend_offer = true;
    }
    if (pm_ui_dirty && pm_resend_offer && pm_file_dirty) break;
  }

  if (g_deferred_sound) {
    uint8_t snd = g_deferred_sound;
    g_deferred_sound = 0;
    if (snd == 1) beep_msg_in();
    else if (snd == 2) beep_msg_out();
    else if (snd == 3) beep_error();
  }

  // Forward decl from display.cpp — set whenever flush_cb resets the
  // PPA after a timeout. Folded into the deferred-work check so the
  // existing lvgl_lock(5) block runs and we get a chance to invalidate
  // the active screen for a fresh redraw.
  extern volatile bool g_ppa_recovery_redraw_pending;
  const bool have_deferred_lvgl_work =
      g_ppa_recovery_redraw_pending ||
      (g_deferred_msg_count > 0) ||
      (g_deferred_msg_dropped > 0) ||
      (g_screen_awake && (g_deferred_refresh_targets ||
       g_deferred_serialmon_dirty || g_deferred_timelabel_dirty ||
       g_deferred_battery_dirty || g_deferred_route_label_dirty ||
       g_deferred_wifi_status_dirty || g_deferred_features_dirty ||
       g_deferred_discover_done)) ||
      g_deferred_receipt_update ||
      g_deferred_repeater_mon_dirty ||
      (g_deferred_repeater_btns != 0) ||
      (g_deferred_status_char && g_pending_status_label) ||
      g_deferred_send_pending ||
      g_dismiss_keyboard ||
      g_deferred_scroll_bottom ||
      g_pending_scroll_to_divider ||
      g_deferred_swipe_back ||
      g_deferred_swipe_home ||
      pm_ui_dirty ||
      pm_resend_offer;

  // ---- Drain deferred LVGL work under the shared LVGL mutex ----
  if (have_deferred_lvgl_work && lvgl_lock(5)) {
    crash_diagnostics_mark(false, CrashStage::UiWork);

  // 0. PPA-recovery: if flush_cb just reset the PPA client because of
  // a timeout, force an invalidation of the whole active screen so the
  // next render replaces whatever the framebuffer was frozen on.
  display_handle_ppa_recovery_if_pending();

  // 1. Incoming chat messages — bounded per iteration. chat_add() does
  //    real LVGL work (flex insert + label measure); processing a burst
  //    of 10+ in a single lock hold produces a visible hitch. Cap at 3
  //    per iteration and shift the rest down; the next loop pass picks
  //    them up 1-2 ms later without a user-visible delay.
  if (g_deferred_msg_count > 0) {
    const int kMaxMsgsPerIter = 3;
    int process_n = g_deferred_msg_count;
    if (process_n > kMaxMsgsPerIter) process_n = kMaxMsgsPerIter;
    for (int i = 0; i < process_n; i++) {
      DeferredChatMsg& msg = g_deferred_msgs[i];
      lv_obj_t* status_label = chat_add(msg.out, msg.txt, msg.live_status, 0,
                                        msg.sig[0] ? msg.sig : nullptr);
      if (msg.out && msg.live_status && msg.msg_ts && g_uimesh) {
        g_uimesh->bindPmStatusLabel(msg.msg_ts, status_label);
      }
    }
    const int remaining = g_deferred_msg_count - process_n;
    if (remaining > 0) {
      // Shift unprocessed messages down so the next iteration starts at [0]
      memmove(&g_deferred_msgs[0], &g_deferred_msgs[process_n],
              remaining * sizeof(g_deferred_msgs[0]));
    }
    g_deferred_msg_count = remaining;
  }
  if (g_deferred_msg_dropped > 0) {
    char dbuf[48];
    snprintf(dbuf, sizeof(dbuf), "(%d messages missed - queue full)", g_deferred_msg_dropped);
    chat_add(false, dbuf);
    g_deferred_msg_dropped = 0;
  }

  // 2. Refresh targets — throttled to 300 ms. Busy channels can fire
  //    g_deferred_refresh_targets 10+ times per second (one per incoming
  //    message); each run does an O(N) contact scan + a full rebuild of
  //    the home list (LVGL widget-create storm). Coalescing to ~3 Hz
  //    keeps the home list visually fresh without rebuilding it per
  //    message. The flag stays set across the throttle window so the
  //    next post-window iteration catches any accumulated changes.
  {
    static uint32_t s_last_refresh_ms = 0;
    lv_obj_t* act = lv_scr_act();
    const bool on_home = (act == ui_homescreen);
    const uint32_t refresh_interval_ms = on_home ? 300 : 1500;
    if (g_screen_awake && g_deferred_refresh_targets &&
        (millis() - s_last_refresh_ms) >= refresh_interval_ms) {
      g_deferred_refresh_targets = false;
      s_last_refresh_ms = millis();
      ui_refresh_targets();
    }
  }

  // 3. Serial monitor label — gated by visibility AND throttled.
  //   - Only refresh when the Settings screen is actually on-screen;
  //     rewrapping a 4 KB text label every time any code path calls
  //     serialmon_append() is expensive (full re-measure + re-render),
  //     and most of the time the user is looking at a different screen.
  //   - Even when Settings is up, we cap refresh to ~3 Hz (333 ms).
  //     Burst log bouts (WiFi scan, advert flood) can fire dozens of
  //     appends per second; forcing a label re-flow on each one stalled
  //     LVGL's flush pipeline visibly.
  {
    static uint32_t s_serialmon_last_refresh_ms = 0;
    const uint32_t SERIALMON_MIN_INTERVAL_MS = 333;
    // Lazily create serialLabel the first time we see dirty data AND
    // ui_serialmonitorwindow exists. Previously, serialmon_append_raw
    // created the label itself, which meant early boot logs (sd_init,
    // emoji_atlas_init, anything before ui_init) were silently
    // dropped because the window didn't exist yet. Now those logs
    // accumulate in g_serial_buf and show up here on the first
    // post-ui_init drain.
    if (g_screen_awake && g_deferred_serialmon_dirty && ui_serialmonitorwindow && !serialLabel) {
      serialLabel = lv_label_create(ui_serialmonitorwindow);
      lv_obj_set_width(serialLabel, lv_pct(100));
      lv_label_set_long_mode(serialLabel, LV_LABEL_LONG_WRAP);
      lv_label_set_text(serialLabel, "");
      // Recolor DISABLED: the draw routine scans every character of
      // the label looking for "#RRGGBB ... #" markers on every paint
      // pass. On a 2 KB log that's ~2000 scans per frame of scroll —
      // visible as sluggish serial-monitor scrolling. We never emit
      // color markers in log lines, so disabling is pure win (and
      // lets us drop the '#' → '##' escape in serialmon_append).
      // Use plain-Montserrat (no Greek/emoji fallback chain) — saves
      // two per-character font lookups on a ~1-2 KB log relayout.
      // Serial messages are ASCII anyway.
      lv_obj_set_style_text_font(serialLabel, &lv_font_plain_14, 0);
    }
    if (g_screen_awake && g_deferred_serialmon_dirty && serialLabel &&
        lv_disp_get_scr_act(NULL) == ui_settingscreen &&
        (millis() - s_serialmon_last_refresh_ms) >= SERIALMON_MIN_INTERVAL_MS) {
      g_deferred_serialmon_dirty = false;
      s_serialmon_last_refresh_ms = millis();
      memcpy(g_serial_buf_front, g_serial_buf, g_serial_len + 1);
      lv_label_set_text(serialLabel, g_serial_buf_front);
      lv_obj_scroll_to_y(ui_serialmonitorwindow, LV_COORD_MAX, LV_ANIM_OFF);
    }
  }

  // 3b. Deferred time label
  if (g_screen_awake && g_deferred_timelabel_dirty) {
    g_deferred_timelabel_dirty = false;
    update_timelabel();
  }
  // 3c. Deferred battery indicator (cached by battery_poll())
  if (g_screen_awake && g_deferred_battery_dirty) {
    g_deferred_battery_dirty = false;
    update_batterylabel();
  }
  if (g_screen_awake && g_deferred_route_label_dirty) {
    g_deferred_route_label_dirty = false;
    chat_update_route_label();
  }
  if (g_screen_awake && g_deferred_wifi_status_dirty) {
    g_deferred_wifi_status_dirty = false;
    wifi_ui_update_status();
  }
  if (g_screen_awake && g_deferred_features_dirty) {
    g_deferred_features_dirty = false;
    features_update_status_labels();
  }
  if (g_screen_awake && g_deferred_discover_done) {
    g_deferred_discover_done = false;
    repeater_populate_dropdown();
  }
  if (g_deferred_receipt_update) {
    g_deferred_receipt_update = false;
    if (g_channel_receipt.status_label) {
      apply_receipt_count_to_label(g_channel_receipt.status_label,
                                   g_channel_receipt.heard_count, 0);
    }
    // Discovery finished — clean up
    g_channel_receipt.active = false;
    g_channel_receipt.discover_tag = 0;
    g_channel_receipt.timeout_ms = 0;
  }

  // 3c. Deferred repeater monitor
  if (g_deferred_repeater_mon_dirty && ui_repeatermonitor) {
    g_deferred_repeater_mon_dirty = false;
    lv_label_set_text(ui_repeatermonitor, g_deferred_repeater_mon);
  }

  // 3d. Deferred repeater action buttons
  if (g_deferred_repeater_btns != 0) {
    bool show = (g_deferred_repeater_btns > 0);
    g_deferred_repeater_btns = 0;
    repeater_set_action_buttons_visible(show);
  }

  // 4a. Per-message PM ring: process dirty entries
  for (int i = 0; i < PM_RING_SIZE; i++) {
    OutboundPM& s = g_pm_ring[i];
    if (!s.active) continue;
    if (s.ui_dirty && s.status_label) {
      apply_status_to_label(s.status_label, s.state);
      s.ui_dirty = false;
    }
    // Show resend button on failed PMs (once)
    if (s.state == 'N' && s.status_label && !s.resend_offered && s.recipient && s.retry_text[0]) {
      chat_add_resend_btn(s.status_label, s.recipient->id.pub_key, s.retry_text);
      s.resend_offered = true;
    }
  }

  // 4b. Channel receipt status (uses g_deferred_status_char + g_pending_status_label)
  if (g_deferred_status_char && g_pending_status_label) {
    apply_status_to_label(g_pending_status_label, g_deferred_status_char);
    g_deferred_status_char = 0;
    g_pending_status_label = nullptr;
  }

  // 5. Outbound message send
  if (g_deferred_send_pending) {
    g_deferred_send_pending = false;
    if (g_uimesh) g_uimesh->sendTextToCurrent(g_deferred_send_text);
    g_deferred_send_text[0] = '\0';
  }

  // 6. Dismiss keyboard
  if (g_dismiss_keyboard) {
    g_dismiss_keyboard = false;
    if (g_in_chat_mode && ui_Keyboard1 &&
        !lv_obj_has_flag(ui_Keyboard1, LV_OBJ_FLAG_HIDDEN)) {
      kb_hide(ui_Keyboard1, ui_textsendtype);
      if (ui_textsendtype) lv_obj_set_y(ui_textsendtype, TEXTSEND_Y_DEFAULT);
      textsend_counter_show();
      chat_scroll_to_newest();
    } else if (repeater_cli_mode_active()) {
      // Repeater CLI keyboard: hide it but stay in CLI mode.
      repeater_cli_hide_keyboard();
    }
  }

  // 7. Deferred scroll-to-bottom
  if (g_deferred_scroll_bottom) {
    g_deferred_scroll_bottom = false;
    lv_obj_update_layout(lv_scr_act());
    chat_scroll_to_newest();
  }

  // 7b. Deferred scroll-to-NEW-MESSAGES-divider
  // Set by load_chat_from_file when it encounters unread messages.
  // We defer to here so LVGL has finished laying out the freshly
  // added bubbles — otherwise divider_y is 0 and the scroll lands
  // at the top instead of the divider.
  //
  // Apply keyboard-aware bottom padding BEFORE reading divider_y —
  // chat_apply_kb_aware_bottom_pad may insert enough pad to keep
  // content clear of the text-input area, and that changes bubble
  // layout relative to the panel origin. Without the pre-scroll pad
  // pass, the user would open a chat-with-unread, see the divider
  // near the top, then the panel would auto-adjust a tick later and
  // the divider jumps visibly. Apply pad first so the first render
  // is already the final layout.
  if (g_pending_scroll_to_divider && ui_chatpanel) {
    lv_obj_t* divider = g_pending_scroll_to_divider;
    g_pending_scroll_to_divider = nullptr;
    // Defence-in-depth against UAF: lv_obj_is_valid scans LVGL's global
    // registry so a freed pointer returns false; the parent check catches
    // the case where the divider was reparented off ui_chatpanel.
    if (lv_obj_is_valid(divider) && lv_obj_get_parent(divider) == ui_chatpanel) {
      chat_apply_kb_aware_bottom_pad();
      lv_obj_update_layout(lv_scr_act());
      lv_coord_t divider_y = lv_obj_get_y(divider);
      const lv_coord_t top_pad = 12;
      lv_coord_t target = divider_y - top_pad;
      if (target < 0) target = 0;
      lv_obj_scroll_to_y(ui_chatpanel, target, LV_ANIM_OFF);
    }
  }

  // 8. Deferred swipe gestures
  if (g_deferred_swipe_back) {
    g_deferred_swipe_back = false;
    if (g_in_chat_mode) {
      exit_chat_mode();
    } else {
      // On any non-home screen, swipe left goes to home
      lv_obj_t* act = lv_scr_act();
      if (act != ui_homescreen) {
        _ui_screen_change(&ui_homescreen, LV_SCR_LOAD_ANIM_NONE, 0, 0,
                           ui_homescreen_screen_init);
      }
    }
  }
  if (g_deferred_swipe_home) {
    g_deferred_swipe_home = false;
    if (g_in_chat_mode) exit_chat_mode();
    lv_obj_t* act = lv_scr_act();
    if (act != ui_homescreen) {
      _ui_screen_change(&ui_homescreen, LV_SCR_LOAD_ANIM_NONE, 0, 0,
                         ui_homescreen_screen_init);
    }
  }

  // --- Single render pass per loop iteration ---
  // Drive LVGL directly from loopTask (v11 parity). This serialises all
  // widget ops above with rendering — the deferred-work section in this loop
  // mutates widgets without taking lvgl_lock(), so running lv_timer_handler()
  // on a separate core 0 task race-corrupts the dirty-area list.
    lvgl_unlock();
  }

  service_radio();
  if (pm_file_dirty) {
    for (int i = 0; i < PM_RING_SIZE; i++) {
      OutboundPM& s = g_pm_ring[i];
      if (!s.active || !s.file_dirty || !s.chat_key[0]) continue;
      esp_task_wdt_reset();
      update_tx_status_by_msg_ts(String(s.chat_key), s.msg_ts, s.state);
      s.file_dirty = false;
    }
  }

  // The early radio pass precedes the deferred bubble drain above.
  // Translation dequeues just one request before servicing more radio
  // callbacks during HTTP waits; those new requests wait for the next
  // iteration and its bubble drain before they can be translated.
  // While UI has pending work (scroll/rebuild/input), reduce network
  // translation pressure to keep gesture/render latency low.
  {
    static uint32_t s_translate_tick_ms = 0;
    const uint32_t now_ms = millis();
    const uint32_t translate_interval_ms =
        ui_interacting ? 1400 : (have_deferred_lvgl_work ? 900 : 350);
    if (g_deferred_msg_count == 0 &&
        (now_ms - s_translate_tick_ms) >= translate_interval_ms) {
      s_translate_tick_ms = now_ms;
      crash_diagnostics_mark(false, CrashStage::Translation);
      translate_loop(service_radio);
    }
  }

  service_radio();

  if (g_uimesh) g_uimesh->checkPmRetryTimeout();
  if (g_uimesh) g_uimesh->checkPmHardTimeout();
  if (g_uimesh) g_uimesh->checkPmUnconfirmedExpiry();

  // Deferred radio actions
  if (g_deferred_flood_advert) {
    g_deferred_flood_advert = false;
    if (g_uimesh) {
      serialmon_append("TX flood advert");
      g_uimesh->sendFloodAdvert(0);
    } else {
      serialmon_append("TX flood advert failed: mesh not ready");
    }
  }
  if (g_deferred_zero_advert)  {
    g_deferred_zero_advert = false;
    if (g_uimesh) {
      serialmon_append("TX 0-hop advert");
      g_uimesh->sendZeroHopAdvert(0);
    } else {
      serialmon_append("TX 0-hop advert failed: mesh not ready");
    }
  }
  if (g_fixed_position_apply_pending) {
    g_fixed_position_apply_pending = false;
    if (g_uimesh) {
      if (g_fixed_position_valid) {
        g_uimesh->setNodePosition(g_fixed_position_lat, g_fixed_position_lon);
      } else {
        g_uimesh->clearNodePosition();
      }
      g_deferred_flood_advert = true;
    }
  }
  if (g_deferred_preset_idx >= 0) { int idx = g_deferred_preset_idx; g_deferred_preset_idx = -1; if (g_uimesh) g_uimesh->applyPresetByIdx(idx, false); }

  rtc_clock.tick();
  crash_diagnostics_mark(false, CrashStage::Network);
  wifi_loop();
  // WiFi-dependent loops don't need to poll at the main-loop rate.
  // During heavy UI work, run them even less often so scroll/render
  // stays smooth without removing any functionality.
  {
    static uint32_t s_wifi_loops_ms = 0;
    uint32_t now_ms = millis();
    const uint32_t wifi_interval_ms =
        ui_interacting ? 1500 : (have_deferred_lvgl_work ? 800 : 250);
    if ((now_ms - s_wifi_loops_ms) >= wifi_interval_ms) {
      s_wifi_loops_ms = now_ms;
      ota_loop();
      webdash_loop();
      tgbridge_loop();
    }
  }
  poll_channel_receipt_if_due();

  if (g_del.kind != TargetKind::NONE &&
      (millis() - g_del.armed_ms) > 2000 && lvgl_lock(5)) {
    // del_warn_clear is in home_ui — but we need the inline version here
    // since home_ui.cpp's del_warn_clear calls build_homescreen_list
    if (ui_searchfield) {
      lv_obj_set_style_text_color(ui_searchfield, lv_color_hex(g_theme->search_text), 0);
      lv_textarea_set_text(ui_searchfield, g_search_filter);
    }
    g_del = {};
    g_long_press_just_fired = false;
    lvgl_unlock();
  }

  // PM ring: evict expired entries. 'U' slots are transitioned to 'N' by
  // checkPmUnconfirmedExpiry() rather than evicted, so we skip them here.
  {
    uint32_t now = millis();
    for (int i = 0; i < PM_RING_SIZE; i++) {
      OutboundPM& s = g_pm_ring[i];
      if (s.active && s.state != 'P' && s.state != 'U' && s.expiry_ms && (int32_t)(now - s.expiry_ms) > 0) {
        s.active = false;
        s.status_label = nullptr;
      }
    }
  }

  if (g_notifications_dirty && lvgl_lock(5)) {
    // Serialize with UI chat loads/flushes; drain text before persisting badges.
    crash_diagnostics_mark(false, CrashStage::Storage, 5);
    const bool text_saved = chat_append_flush_sync();
    lvgl_unlock();
    if (text_saved) {
      crash_diagnostics_mark(false, CrashStage::Storage, 1);
      g_notifications_dirty = false;
      save_notifications_nvs();
    }
    crash_diagnostics_mark(false, CrashStage::Idle);
  }

  // Debounced contact save
  if (g_contacts_save_dirty && (millis() - g_contacts_save_ms) > 5000) {
    crash_diagnostics_mark(false, CrashStage::Storage, 2);
    g_contacts_save_dirty = false;
    esp_task_wdt_reset();
    if (g_uimesh) g_uimesh->saveContactsNow();
    crash_diagnostics_mark(false, CrashStage::Idle);
  }

  // Debounced UI-prefs save. save_ui_prefs_nvs() just marks dirty; we
  // wait 500 ms of quiet before writing 13 NVS keys to flash. Rapid
  // toggle fans (user flipping Notify / Auto-Contact / Speaker in a
  // burst) become a single write instead of N.
  if (g_ui_prefs_save_dirty && (millis() - g_ui_prefs_save_ms) > 500) {
    crash_diagnostics_mark(false, CrashStage::Storage, 3);
    g_ui_prefs_save_dirty = false;
    save_ui_prefs_nvs_now();
    crash_diagnostics_mark(false, CrashStage::Idle);
  }

  // Debounced map-viewport save. Pan/zoom fires this flag; we coalesce
  // rapid pan bursts into one NVS write.
  extern void map_viewport_save_if_dirty_flush();
  if (g_map_viewport_save_dirty && (millis() - g_map_viewport_save_ms) > 500) {
    crash_diagnostics_mark(false, CrashStage::Storage, 4);
    g_map_viewport_save_dirty = false;
    map_viewport_save_if_dirty_flush();
    crash_diagnostics_mark(false, CrashStage::Idle);
  }

  // Coalesced chat-line append flush. append_chat_to_file() buffers
  // formatted lines and arms g_chat_flush_deadline_ms ~200 ms out;
  // drain once the deadline elapses so busy channels don't turn into
  // one flash open/append/close per incoming message.
  if (g_chat_flush_deadline_ms != 0 &&
      (int32_t)(millis() - g_chat_flush_deadline_ms) >= 0 && lvgl_lock(5)) {
    crash_diagnostics_mark(false, CrashStage::Storage, 5);
    chat_append_flush_if_due();
    lvgl_unlock();
    crash_diagnostics_mark(false, CrashStage::Idle);
  }

  crash_diagnostics_mark(false, CrashStage::Network, 1);
  bool login_pending = (g_login_pending_key[0] | g_login_pending_key[1] |
                        g_login_pending_key[2] | g_login_pending_key[3]) != 0;
  if (login_pending && g_login_timeout_ms && (int32_t)(millis() - g_login_timeout_ms) > 0) {
    g_login_timeout_ms = 0;
    // Re-resolve the repeater pointer fresh from the SAVED pubkey
    // snapshot (repeater_ui.cpp), never via g_selected_repeater. The
    // snapshot is the canonical record; g_selected_repeater is a
    // short-lived pointer and may already be dangling here if a
    // contact deletion happened between the original login send and
    // this retry tick.
    ContactInfo* fresh = nullptr;
    uint8_t pubkey[32];
    if (!g_repeater_logged_in && g_mesh && g_login_retry_count < 4 &&
        repeater_get_selected_pubkey(pubkey)) {
      // Selection changed while a previous login was pending: hard-cancel
      // that old retry stream.
      if (memcmp(g_login_pending_key, pubkey, 4) != 0) {
        memset(g_login_pending_key, 0, 4);
        g_login_retry_count = 0;
        g_login_last_pw[0] = '\0';
        repeater_update_monitor("Login attempt canceled (new repeater selected).");
        return;
      }
      fresh = g_mesh->lookupContactByPubKey(pubkey, 32);
    }
    // Dashboard-originated logins may not have a repeater-screen selection
    // snapshot yet. Fall back to the saved pending key so retries still work.
    if (!fresh && !g_repeater_logged_in && g_mesh && g_login_retry_count < 4) {
      uint8_t key4[4] = { g_login_pending_key[0], g_login_pending_key[1],
                          g_login_pending_key[2], g_login_pending_key[3] };
      if ((key4[0] | key4[1] | key4[2] | key4[3]) != 0) {
        ContactsIterator it = g_mesh->startContactsIterator();
        ContactInfo c;
        while (it.hasNext(g_mesh, c)) {
          if (c.type == ADV_TYPE_REPEATER && memcmp(c.id.pub_key, key4, 4) == 0) {
            fresh = g_mesh->lookupContactByPubKey(c.id.pub_key, 32);
            break;
          }
        }
      }
    }
    if (fresh) {
      g_login_retry_count++;
      ContactInfo retry_target = *fresh;
      const bool have_path = (fresh->out_path_len != OUT_PATH_UNKNOWN);
      const bool use_flood = !have_path || ((g_login_retry_count % 2) == 0);
      if (use_flood) retry_target.out_path_len = OUT_PATH_UNKNOWN;
      uint32_t est_timeout;
      int result = g_mesh->sendLogin(retry_target, g_login_last_pw, est_timeout);
      if (result != MSG_SEND_FAILED) {
        g_selected_repeater = fresh;
        memcpy(g_login_pending_key, fresh->id.pub_key, 4);
        g_login_timeout_ms = millis() + max(est_timeout * 4, (uint32_t)20000);
        char msg[64];
        snprintf(msg, sizeof(msg), "Retry %d/4 via %s - waiting...",
                 g_login_retry_count, use_flood ? "flood" : "path");
        repeater_update_monitor(msg);
        return;
      }
    }
    memset(g_login_pending_key, 0, 4);
    if (!g_repeater_logged_in)
      repeater_update_monitor("No response - tap Login to retry.");
  }

  // ---- Frame rate limiter ----
  report_rx_paths_if_changed();
  service_radio();
  crash_diagnostics_mark(false, CrashStage::Sleep);
  cpu_light_sleep_poll();
  crash_diagnostics_mark(false, CrashStage::Idle);
  delay(1);
}

// ============================================================
// mesh_api wrappers — let extracted modules call UIMesh methods
// ============================================================

bool mesh_try_get_current_rssi(float& rssi) {
  if (!g_mesh || !mesh_try_lock(0)) return false;
  rssi = radio_driver.getCurrentRSSI();
  mesh_unlock();
  return true;
}

static void report_rx_paths_if_changed() {
  static uint32_t last_report_ms = 0;
  static uint32_t last_raw = 0;
  const uint32_t now = millis();
  if ((uint32_t)(now - last_report_ms) < 60000 || !mesh_try_lock(0)) return;
  const RxPathCounters snapshot = s_rx_paths;
  mesh_unlock();
  last_report_ms = now;
  const uint32_t total = snapshot.raw[0] + snapshot.raw[1] + snapshot.raw[2] + snapshot.raw[3]
                        + snapshot.short_metadata;
  if (total == last_raw) return;
  last_raw = total;
  Serial.printf("[rx-path] group text raw/parsed/decrypted/accepted: 1B=%lu/%lu/%lu/%lu 2B=%lu/%lu/%lu/%lu 3B=%lu/%lu/%lu/%lu; reserved=%lu short=%lu version=%lu\n",
      (unsigned long)snapshot.raw[0], (unsigned long)snapshot.parsed[0],
      (unsigned long)snapshot.decrypted[0], (unsigned long)snapshot.accepted[0],
      (unsigned long)snapshot.raw[1], (unsigned long)snapshot.parsed[1],
      (unsigned long)snapshot.decrypted[1], (unsigned long)snapshot.accepted[1],
      (unsigned long)snapshot.raw[2], (unsigned long)snapshot.parsed[2],
      (unsigned long)snapshot.decrypted[2], (unsigned long)snapshot.accepted[2],
      (unsigned long)snapshot.raw[3], (unsigned long)snapshot.short_metadata,
      (unsigned long)snapshot.unsupported_version);
}

String mesh_rx_report() {
  struct Snapshot {
    uint32_t received, errors, sent, start_errors, gap, last_service, duplicates;
    int read_error, start_error;
    bool pending;
    uint32_t accepted[MAX_GROUP_CHANNELS], epoch[MAX_GROUP_CHANNELS];
    RxPathCounters paths;
  } snapshot{};
  if (!g_mesh || !mesh_try_lock(pdMS_TO_TICKS(5))) return "Radio busy; retry shortly.\n";
  snapshot.received = radio_driver.getPacketsRecv();
  snapshot.errors = radio_driver.getPacketsRecvErrors();
  snapshot.sent = radio_driver.getPacketsSent();
  snapshot.start_errors = radio_driver.getRxStartErrors();
  snapshot.read_error = radio_driver.getLastRxReadError();
  snapshot.start_error = radio_driver.getLastRxStartError();
  snapshot.pending = radio_driver.hasPendingReceiveInterrupt();
  snapshot.gap = s_rx_max_service_gap_ms;
  snapshot.last_service = s_rx_last_service_ms;
  snapshot.duplicates = s_rx_channel_duplicates;
  snapshot.paths = s_rx_paths;
  memcpy(snapshot.accepted, s_rx_channel_accepted, sizeof(snapshot.accepted));
  memcpy(snapshot.epoch, s_rx_channel_last_epoch, sizeof(snapshot.epoch));
  mesh_unlock();
  char line[320];
  snprintf(line, sizeof(line), "uptime_s=%lu screen=%s\nreceived=%lu read_errors=%lu last_read_error=%d sent=%lu\nRX_start_errors=%lu last_start_error=%d pending_IRQ=%u\nmax_service_gap_ms=%lu last_service_age_ms=%lu channel_duplicates=%lu\n",
      (unsigned long)(millis() / 1000), g_screen_awake ? "on" : "off",
      (unsigned long)snapshot.received, (unsigned long)snapshot.errors, snapshot.read_error,
      (unsigned long)snapshot.sent, (unsigned long)snapshot.start_errors, snapshot.start_error,
      snapshot.pending ? 1u : 0u, (unsigned long)snapshot.gap,
      (unsigned long)(millis() - snapshot.last_service), (unsigned long)snapshot.duplicates);
  String report(line);
  report += "Counts since reboot; last_read_error is the last failure, not current radio status.\n";
  for (int width = 0; width < 3; ++width) {
    snprintf(line, sizeof(line), "path_hash_bytes=%d group_raw=%lu parsed=%lu decrypted=%lu accepted=%lu\n",
        width + 1, (unsigned long)snapshot.paths.raw[width],
        (unsigned long)snapshot.paths.parsed[width], (unsigned long)snapshot.paths.decrypted[width],
        (unsigned long)snapshot.paths.accepted[width]);
    report += line;
  }
  snprintf(line, sizeof(line), "group_reserved_path=%lu short_metadata=%lu unsupported_payload_version=%lu\n",
      (unsigned long)snapshot.paths.raw[3], (unsigned long)snapshot.paths.short_metadata,
      (unsigned long)snapshot.paths.unsupported_version);
  report += line;
  for (int i = 0; i < MAX_GROUP_CHANNELS; ++i) {
    if (!snapshot.accepted[i]) continue;
    snprintf(line, sizeof(line), "channel=%d accepted=%lu last_receive_epoch=%lu\n", i,
             (unsigned long)snapshot.accepted[i], (unsigned long)snapshot.epoch[i]);
    report += line;
  }
  return report;
}

void mesh_set_tx_power_pref(int8_t dbm)               { if (g_uimesh) g_uimesh->setTxPowerPref(dbm); }
void mesh_set_text_header_bytes(uint8_t bytes)        { if (g_uimesh) g_uimesh->setTextHeaderBytes(bytes); }
void mesh_set_path_hash_bytes(uint8_t bytes)          { if (g_uimesh) g_uimesh->setPathHashBytes(bytes); }
uint8_t mesh_get_path_hash_bytes()                    { return g_uimesh ? g_uimesh->getPathHashBytes() : 1; }
void mesh_send_zero_hop_advert(int delay_ms)           { if (g_uimesh) g_uimesh->sendZeroHopAdvert(delay_ms); }
uint32_t mesh_send_discover_repeaters() {
  if (!g_uimesh) return 0;
#if defined(ESP32)
  if (!mesh_try_lock(pdMS_TO_TICKS(20))) return 0;
#endif
  const uint32_t tag = g_manual_discover_active ? 0 : g_uimesh->sendDiscoverRepeaters();
#if defined(ESP32)
  mesh_unlock();
#endif
  return tag;
}

uint32_t mesh_begin_repeater_discovery() {
  if (!g_uimesh) return 0;
#if defined(ESP32)
  if (!mesh_try_lock(pdMS_TO_TICKS(20))) return 0;
#endif
  uint32_t tag = 0;
  if (!g_manual_discover_active) {
    mesh_clear_discovery_results();
    g_uimesh->sendFloodAdvert(100);
    tag = g_uimesh->sendDiscoverRepeaters();
    g_discover_tag = tag;
    g_manual_discover_active = tag != 0;
  }
#if defined(ESP32)
  mesh_unlock();
#endif
  return tag;
}

bool mesh_finish_repeater_discovery() {
#if defined(ESP32)
  if (!mesh_try_lock(0)) return false;
#endif
  g_manual_discover_active = false;
  g_discover_tag = 0;
#if defined(ESP32)
  mesh_unlock();
#endif
  return true;
}
void mesh_clear_discovery_results() {
#if defined(ESP32)
  portENTER_CRITICAL(&s_discovery_mux);
#endif
  s_discovery_results.clear();
#if defined(ESP32)
  portEXIT_CRITICAL(&s_discovery_mux);
#endif
}
int mesh_get_discovery_results(DiscoveryResult* out, int capacity) {
#if defined(ESP32)
  portENTER_CRITICAL(&s_discovery_mux);
#endif
  const int count = s_discovery_results.snapshot(out, capacity);
#if defined(ESP32)
  portEXIT_CRITICAL(&s_discovery_mux);
#endif
  return count;
}
bool mesh_get_discovery_result(const uint8_t* key, DiscoveryResult& out) {
#if defined(ESP32)
  portENTER_CRITICAL(&s_discovery_mux);
#endif
  const bool found = s_discovery_results.find(key, out);
#if defined(ESP32)
  portEXIT_CRITICAL(&s_discovery_mux);
#endif
  return found;
}
void mesh_send_flood_advert(int delay_ms)              { if (g_uimesh) g_uimesh->sendFloodAdvert(delay_ms); }
void mesh_rename_if_non_empty(const char* name)        { if (g_uimesh) g_uimesh->renameIfNonEmpty(name); }
int  mesh_join_hashtag_channel(const String& raw)      { return g_uimesh ? g_uimesh->joinHashtagChannel(raw) : -1; }
int  mesh_join_private_channel(const String& name, const String& hex_key) {
  if (!g_uimesh) return -1;
  String raw = name;
  String key = hex_key;
  raw.trim();
  key.trim();
  if (!raw.length() || !key.length()) return -1;
  raw += "|";
  raw += key;
  return g_uimesh->joinHashtagChannel(raw);
}
void mesh_purge_contacts_and_repeaters()               { if (g_uimesh) g_uimesh->purgeContactsAndRepeaters(); }
int  mesh_current_channel_idx()                        { return g_uimesh ? g_uimesh->currentChannelIdx() : -1; }
bool mesh_select_contact_by_pubkey(const uint8_t* pk)  { return g_uimesh ? g_uimesh->selectContactByPubKey(pk) : false; }
bool mesh_select_channel_by_idx(int idx)               { return g_uimesh ? g_uimesh->selectChannelByIdx(idx) : false; }
bool mesh_delete_contact_by_pubkey(const uint8_t* pk)  { return g_uimesh ? g_uimesh->deleteContactByPubKey(pk) : false; }
bool mesh_delete_repeater_by_pubkey(const uint8_t* pk) { return g_uimesh ? g_uimesh->deleteRepeaterByPubKey(pk) : false; }
bool mesh_reset_repeater_path_by_pubkey(const uint8_t* pk) { return g_uimesh ? g_uimesh->resetRepeaterPathByPubKey(pk) : false; }
bool mesh_delete_channel_by_idx(int idx)               { return g_uimesh ? g_uimesh->deleteChannelByIdx(idx) : false; }
bool mesh_delete_selected_channel()                    { return g_uimesh ? g_uimesh->deleteSelectedChannel() : false; }
void mesh_stop_repeater_connection(const uint8_t* pk)  { if (g_uimesh) g_uimesh->stopRepeaterConnection(pk); }
const Preset* mesh_presets(int& count)                 { return UIMesh::presets(count); }
void mesh_apply_preset_by_idx(int idx, bool do_advert) { if (g_uimesh) g_uimesh->applyPresetByIdx(idx, do_advert); }

void mesh_reset_current_contact_path() {
  if (!g_uimesh) return;
  ContactInfo* c = g_uimesh->currentRecipient();
  if (!c) return;
  c->out_path_len = OUT_PATH_UNKNOWN;
  g_chat_route_path_len = OUT_PATH_UNKNOWN;
  g_deferred_route_label_dirty = true;
  g_contacts_save_dirty = true;
  g_contacts_save_ms = millis();
  serialmon_append("Path reset to flood");
}

bool mesh_discover_contact_path() {
  return g_uimesh ? g_uimesh->discoverContactPath() : false;
}

bool mesh_send_text_to_contact(const uint8_t* pub_key, const char* text) {
  return g_uimesh ? g_uimesh->sendTextToContactByPubKey(pub_key, text) : false;
}
bool mesh_send_text_to_channel(int idx, const char* text) {
  return g_uimesh ? g_uimesh->sendTextToChannelByIdx(idx, text) : false;
}
bool mesh_resend_pm_by_bubble_label(struct _lv_obj_t* status_label) {
  return g_uimesh ? g_uimesh->resendByStatusLabel((lv_obj_t*)status_label) : false;
}
const char* mesh_get_node_name() {
  return g_uimesh ? g_uimesh->getNodeName() : "DHE04005D";
}
void mesh_get_fixed_position(double* lat, double* lon) {
  if (lat) *lat = g_fixed_position_valid ? g_fixed_position_lat : 0.0;
  if (lon) *lon = g_fixed_position_valid ? g_fixed_position_lon : 0.0;
}
bool mesh_set_fixed_position(double lat, double lon) {
  bool saved = save_fixed_position_nvs(lat, lon);
  if (saved) {
    g_fixed_position_valid = (lat != 0.0 || lon != 0.0);
    g_fixed_position_lat = g_fixed_position_valid ? lat : 0.0;
    g_fixed_position_lon = g_fixed_position_valid ? lon : 0.0;
    g_fixed_position_apply_pending = true;
  }
  serialmon_append(saved ? "Fixed position saved" : "Fixed position save failed");
  return saved;
}
bool mesh_clear_fixed_position() {
  bool saved = clear_fixed_position_nvs();
  if (saved) {
    g_fixed_position_valid = false;
    g_fixed_position_lat = 0.0;
    g_fixed_position_lon = 0.0;
    g_fixed_position_apply_pending = true;
  }
  serialmon_append(saved ? "Fixed position cleared" : "Fixed position clear failed");
  return saved;
}

void mesh_populate_repeater_list() {
  if (!g_uimesh) return;
  g_repeater_count = 0;
  ContactsIterator it = g_uimesh->startContactsIterator();
  ContactInfo c;
  while (it.hasNext(g_uimesh, c)) {
    if (c.type == ADV_TYPE_REPEATER && g_repeater_count < MAX_DD_ENTRIES) {
      RepeaterListEntry& e = g_repeater_list[g_repeater_count++];
      memcpy(e.pub_key, c.id.pub_key, 32);
      strncpy(e.name, c.name, sizeof(e.name) - 1);
      e.name[sizeof(e.name) - 1] = '\0';
      e.out_path_len = c.out_path_len;
    }
  }
}

int mesh_repeater_login(const uint8_t* pub_key, const char* password) {
  if (!g_uimesh) return -1;
  ContactInfo* c = g_uimesh->lookupContactByPubKey(pub_key, 32);
  if (!c) return -1;
  uint32_t est_timeout = 0;

  // Match the robust in-device repeater login flow:
  // 1) pre-discover only when path is unknown
  // 2) send login on current route decision
  // 3) if route exists, also send a flood assist copy
  if (c->out_path_len == OUT_PATH_UNKNOWN) {
    uint8_t req_data[9];
    req_data[0] = 0x03;      // REQ_TYPE_GET_TELEMETRY_DATA
    req_data[1] = ~(0x01);   // request base telemetry only
    req_data[2] = 0;
    req_data[3] = 0;
    req_data[4] = 0;
    req_data[5] = (uint8_t)random(0x100);
    req_data[6] = (uint8_t)random(0x100);
    req_data[7] = (uint8_t)random(0x100);
    req_data[8] = (uint8_t)random(0x100);
    ContactInfo discover_target = *c;
    discover_target.out_path_len = OUT_PATH_UNKNOWN;
    uint32_t dtag = 0, dtimeout = 0;
    (void)g_uimesh->sendRequest(discover_target, req_data, sizeof(req_data), dtag, dtimeout);
  }

  int result = g_uimesh->sendLogin(*c, password, est_timeout);
  if (result == MSG_SEND_FAILED) {
    // Queue retry path instead of hard-failing immediately.
    memcpy(g_login_pending_key, pub_key, 4);
    g_login_retry_count = 0;
    g_login_timeout_ms = millis() + 15000;
    strncpy(g_login_last_pw, password ? password : "", sizeof(g_login_last_pw) - 1);
    g_login_last_pw[sizeof(g_login_last_pw) - 1] = '\0';
    repeater_update_monitor("Radio busy - login queued, retrying...");
    return 0;
  }

  if (c->out_path_len != OUT_PATH_UNKNOWN && c->out_path_len > 0) {
    ContactInfo flood_target = *c;
    flood_target.out_path_len = OUT_PATH_UNKNOWN;
    uint32_t est_timeout_flood = 0;
    int flood_result = g_uimesh->sendLogin(flood_target, password, est_timeout_flood);
    if (flood_result != MSG_SEND_FAILED && est_timeout_flood > est_timeout) {
      est_timeout = est_timeout_flood;
    }
  }

  g_selected_repeater = c;
  memcpy(g_login_pending_key, pub_key, 4);
  g_login_retry_count = 0;
  g_login_timeout_ms = millis() + max(est_timeout * 4, (uint32_t)20000);
  strncpy(g_login_last_pw, password, sizeof(g_login_last_pw) - 1);
  g_login_last_pw[sizeof(g_login_last_pw) - 1] = '\0';
  repeater_update_monitor("Login sent, waiting for response...");
  return 0;
}

// Each of these wrappers takes the target pubkey as an argument (web
// dashboard sends the user-clicked pubkey directly). Resolve to a
// fresh ContactInfo* via lookupContactByPubKey on every call — never
// rely on g_selected_repeater here, since the dashboard request can
// race with a UI-side dropdown change or a contact deletion. The
// short g_selected_repeater store at the bottom of each is a courtesy
// for any UI handler that wakes up next, not a correctness contract.
int mesh_repeater_request_status(const uint8_t* pub_key) {
  if (!g_uimesh || !g_repeater_logged_in) return -1;
  ContactInfo* c = g_uimesh->lookupContactByPubKey(pub_key, 32);
  if (!c) return -1;
  uint32_t tag, est_timeout;
  g_uimesh->sendRequest(*c, (uint8_t)0x01, tag, est_timeout);
  g_status_pending_key = tag;
  g_selected_repeater  = c;
  repeater_update_monitor("Refreshing status...");
  return 0;
}


static int request_repeater_neighbours(const uint8_t* pub_key, bool show_map) {
  if (!g_uimesh || !pub_key || !g_repeater_logged_in ||
      !mesh_try_lock(pdMS_TO_TICKS(20))) return -1;
  ContactInfo* contact = g_uimesh->lookupContactByPubKey(pub_key, 32);
  if (!contact || contact->type != ADV_TYPE_REPEATER) {
    mesh_unlock();
    return -1;
  }
  uint8_t source[32];
  char name[32];
  memcpy(source, contact->id.pub_key, sizeof(source));
  memcpy(name, contact->name, sizeof(name));
  name[31] = '\0';
  // Prefix/SNR snapshot for display only: never add or modify contacts.
  uint8_t request[11] = {0x06, 0, 20, 0, 0, 0, 4, 0, 0, 0, 0};
  uint32_t tag = 0, timeout = 60000;
  const int result = g_uimesh->sendRequest(*contact, request, sizeof(request), tag, timeout);
  if (result != MSG_SEND_FAILED) {
    g_neighbours_pending_key = tag;
    g_selected_repeater = contact;
  }
  mesh_unlock();
  if (result == MSG_SEND_FAILED) return -1;
  // The UI caller already owns LVGL. Web requests do not touch the map.
  if (show_map) map_show_neighbours(tag, source, name, timeout);
  if (show_map)
    Serial.printf("[neighbour-diag] request: version=%u count=%u offset=0 order=%u prefix_bytes=%u; reply header=8 stride=9 (key=4 age=4 SNR=1); compare keys, not names\n",
                  request[1], request[2], request[5], request[6]);
  repeater_update_monitor("Neighbours request sent, waiting...");
  return 0;
}

int mesh_repeater_show_neighbours(const uint8_t* pub_key) {
  return request_repeater_neighbours(pub_key, true);
}

int mesh_repeater_request_neighbours(const uint8_t* pub_key) {
  return request_repeater_neighbours(pub_key, false);
}

int mesh_repeater_send_advert(const uint8_t* pub_key) {
  if (!g_uimesh || !g_repeater_logged_in) return -1;
  ContactInfo* c = g_uimesh->lookupContactByPubKey(pub_key, 32);
  if (!c) return -1;
  uint32_t est_timeout;
  g_uimesh->sendCommandData(*c,
    g_uimesh->getRTCClock()->getCurrentTime(), 0, "advert", est_timeout);
  g_selected_repeater = c;
  repeater_update_monitor("Advert command sent.");
  return 0;
}

int mesh_repeater_send_reboot(const uint8_t* pub_key) {
  if (!g_uimesh || !g_repeater_logged_in) return -1;
  ContactInfo* c = g_uimesh->lookupContactByPubKey(pub_key, 32);
  if (!c) return -1;
  uint32_t est_timeout;
  g_uimesh->sendCommandData(*c,
    g_uimesh->getRTCClock()->getCurrentTime(), 0, "reboot", est_timeout);
  g_selected_repeater = c;
  repeater_update_monitor("Reboot command sent.");
  return 0;
}
