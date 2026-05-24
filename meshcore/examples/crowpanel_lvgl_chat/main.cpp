// ============================================================
// main.cpp — UIMesh class, setup(), loop(), global definitions
// ============================================================

#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>
#include <extra/widgets/keyboard/lv_keyboard.h>
#include "LovyanGFX_Driver.h"
#include <esp_task_wdt.h>

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
  #include <SPIFFS.h>
  #include <Preferences.h>
#endif

#include <helpers/ArduinoHelpers.h>
#include <helpers/StaticPoolPacketManager.h>
#include <helpers/SimpleMeshTables.h>
#include <helpers/IdentityStore.h>
#include <helpers/BaseChatMesh.h>
#include <helpers/TransportKeyStore.h>

#include <RTClib.h>
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
#include "mesh_api.h"
#include "wifi_ntp.h"
#include "features_ui.h"
#include "features_cb.h"
#include "ota_update.h"
#include "web_dashboard.h"
#include "telegram_bridge.h"
#include "translate.h"

// ============================================================
// Global variable DEFINITIONS
// (declared extern in app_globals.h, defined here)
// ============================================================

LGFX gfx;

// ---- Display / LVGL ----
lv_obj_t *serialLabel            = nullptr;
lv_obj_t *g_pending_status_label = nullptr;  // channel receipts only
lv_obj_t *g_scroll_btn           = nullptr;
lv_obj_t *g_chat_header_label    = nullptr;
lv_obj_t *g_chat_route_label     = nullptr;
char      g_chat_target_name[64] = "";
bool      g_in_chat_mode         = false;
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

// Deferred ACK/sound
char   g_deferred_status_char    = 0;
uint8_t g_deferred_sound = 0;

// Deferred LVGL operations
bool   g_deferred_refresh_targets = false;
bool   g_deferred_serialmon_dirty = false;
bool   g_deferred_timelabel_dirty = false;
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
bool g_auto_translate_enabled = false;
int  g_translate_lang_idx     = 0;
bool g_manual_discover_active = false;
uint32_t g_discover_tag = 0;
bool g_deferred_discover_done = false;
char g_discover_names[MAX_DISCOVER_RESULTS][32] = {};
int  g_discover_result_count = 0;
bool g_purged_this_session   = false;
bool g_speaker_enabled = true;

// ---- Notification tracking ----
ContactUnread g_contact_unread[MAX_UNREAD_SLOTS] = {};
ChannelUnread g_channel_unread[MAX_UNREAD_SLOTS] = {};

// ---- SNR tracking ----
ContactSNR g_contact_snr[MAX_UNREAD_SLOTS] = {};

// ---- Chat route indicator ----
uint8_t  g_chat_route_path_len = OUT_PATH_UNKNOWN;
bool     g_chat_route_is_contact = false;

// ---- RTC / Time ----
RTC_PCF8563 g_rtc;
bool g_rtc_ok = false;
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
ContactInfo* g_repeater_list[MAX_DD_ENTRIES] = {};
int          g_repeater_count = 0;

// ---- Serial monitor buffers ----
const size_t SERIAL_BUF_SIZE = 4096;
const size_t SERIAL_BUF_TRIM = SERIAL_BUF_SIZE / 2;
char  g_serial_buf[4096 + 1];
char  g_serial_buf_front[4096 + 1];
size_t g_serial_len = 0;

// ---- Speaker / Discover buttons ----
lv_obj_t* g_speaker_btn = nullptr;
lv_obj_t* g_discover_repeaters_btn = nullptr;
lv_obj_t* g_pkt_fwd_btn = nullptr;

// ---- Keyboard state ----
bool g_kb_greek = false;

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
StdRNG fast_rng;
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
    strncpy(_prefs.node_name, "CrowPanel", sizeof(_prefs.node_name));
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

  void loadContacts() {
    if (!_fs->exists("/contacts")) return;
#if defined(RP2040_PLATFORM)
    File file = _fs->open("/contacts", "r");
#else
    File file = _fs->open("/contacts");
#endif
    if (!file) return;

    ContactRecord rec;
    while (file.read((uint8_t*)&rec, sizeof(rec)) == sizeof(rec)) {
      ContactInfo c;
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
      addContact(c);
    }
    file.close();
  }

  void saveContacts() {
#if defined(NRF52_PLATFORM)
    _fs->remove("/contacts");
    File file = _fs->open("/contacts", FILE_O_WRITE);
#elif defined(RP2040_PLATFORM)
    File file = _fs->open("/contacts", "w");
#else
    File file = _fs->open("/contacts", "w", true);
#endif
    if (!file) return;

    ContactsIterator iter;
    ContactInfo c;
    while (iter.hasNext(this, c)) {
      sanitize_ascii_inplace(c.name);

      ContactRecord rec;
      memcpy(rec.pub_key, c.id.pub_key, 32);
      memcpy(rec.name, c.name, 32);
      rec.type                  = c.type;
      rec.flags                 = c.flags;
      rec.out_path_len          = c.out_path_len;
      rec.last_advert_timestamp = c.last_advert_timestamp;
      memcpy(rec.out_path, c.out_path, 64);
      rec.gps_lat = c.gps_lat;
      rec.gps_lon = c.gps_lon;
      if (file.write((const uint8_t*)&rec, sizeof(rec)) != sizeof(rec)) break;
    }
    file.close();
  }

  void ui_set_devname() {
    if (ui_devicenamelabel) lv_label_set_text(ui_devicenamelabel, _prefs.node_name);
  }

  void saveChannelsToFS() {
#if defined(ESP32)
    File f = SPIFFS.open(CHANNELS_FILE, FILE_WRITE);
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
    if (!SPIFFS.exists(CHANNELS_FILE)) return;

    File f = SPIFFS.open(CHANNELS_FILE, FILE_READ);
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
  bool allowPacketForward(const mesh::Packet*) override { return g_packet_forward_enabled; }
  // Send one extra ACK ~300ms after the primary when a direct path is known.
  // Improves delivery-confirmation reliability on lossy return paths without
  // materially impacting airtime. Flood ACKs aren't duplicated (the ACK is
  // encoded in the path-return packet by BaseChatMesh).
  uint8_t getExtraAckTransmitCount() const override { return 1; }

  void sendFloodScoped(const ContactInfo& recipient, mesh::Packet* pkt, uint32_t delay_millis = 0) override {
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

      bool is_manual  = (g_discover_tag != 0 && tag == g_discover_tag);
      bool is_receipt = (g_channel_receipt.discover_tag != 0 &&
                         tag == g_channel_receipt.discover_tag);
      if (!is_manual && !is_receipt) return;

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

        if (existing) {
          if (g_discover_result_count < MAX_DISCOVER_RESULTS) {
            strncpy(g_discover_names[g_discover_result_count],
                    existing->name, 31);
            g_discover_names[g_discover_result_count][31] = '\0';
            g_discover_result_count++;
          }
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

    char target_key[11];
    snprintf(target_key, sizeof(target_key), "c:%02x%02x%02x%02x",
             contact.id.pub_key[0], contact.id.pub_key[1],
             contact.id.pub_key[2], contact.id.pub_key[3]);

    // Forward to bridges (incoming PM: sender=contact, recipient=me)
    tgbridge_forward_pm(safeName.c_str(), _prefs.node_name, safeText.c_str(), false);
    webdash_broadcast_message(safeName.c_str(), safeText.c_str(), false, target_key);

    // Auto-translate incoming PM if enabled
    if (g_auto_translate_enabled && g_wifi_connected) {
      String ck = key_for_contact(contact.id);
      translate_request_to_file(safeText.c_str(), ck.c_str(), nullptr);
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
    wake_on_event();
    String safeName = sanitize_ascii_string(contact.name);
    String safeText = sanitize_ascii_string(text);

    char logline[260];
    snprintf(logline, sizeof(logline), "RX CMD %s: %s", safeName.c_str(), safeText.c_str());
    serialmon_append(logline);
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

    // Auto-translate incoming channel message if enabled
    // Extract just the message body (strip "SenderName: ")
    if (g_auto_translate_enabled && g_wifi_connected) {
      String ck = key_for_channel(idx);
      int sep2 = safeText.indexOf(": ");
      const char* body_to_translate = (sep2 > 0) ? safeText.c_str() + sep2 + 2 : safeText.c_str();
      translate_request_to_file(body_to_translate, ck.c_str(), nullptr);
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

        for (int i = 0; i < result_count && p + entry_sz <= end; i++, p += entry_sz) {
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
        }
        repeater_update_monitor(buf);
      }
    }
  }

public:
  UIMesh(mesh::Radio& radio, mesh::RNG& rng, mesh::RTCClock& rtc, mesh::MeshTables& tables)
    : BaseChatMesh(radio, *new ArduinoMillis(), rng, rtc,
                   *(new StaticPoolPacketManager(16)), tables) {
    _fs = nullptr;
    _public = nullptr;
    _public_idx = -1;
    _curr_recipient  = nullptr;
    _curr_kind       = TargetKind::NONE;
    _curr_channel_idx= -1;
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
    sendFloodAdvert(250);
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

  uint32_t sendDiscoverRepeaters() {
    uint32_t tag = (uint32_t)millis() ^ ((uint32_t)random(0xFFFF) << 16);
    uint8_t data[10];
    data[0] = 0x80;  // CTL_TYPE_NODE_DISCOVER_REQ, no prefix_only flag
    data[1] = (1 << ADV_TYPE_REPEATER);  // filter: repeaters only
    memcpy(&data[2], &tag, 4);
    uint32_t since = 0;  // all repeaters
    memcpy(&data[6], &since, 4);

    auto pkt = createControlData(data, sizeof(data));
    if (pkt) {
      sendZeroHop(pkt);
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

        // Show pending status, then start live discovery to count nearby repeaters
        char sc = 'P';
        lv_obj_t* lbl = chat_add(true, bubble, false, sc);

        clear_channel_receipt_poll();
        uint32_t rtag = sendDiscoverRepeaters();
        if (rtag && lbl) {
          g_channel_receipt.active = true;
          g_channel_receipt.status_label = lbl;
          g_channel_receipt.chat_key = key;
          g_channel_receipt.discover_tag = rtag;
          g_channel_receipt.timeout_ms = millis() + 4000;
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
      if (SPIFFS.exists(CHANNELS_FILE)) {
        bool ok = true;
        File in = SPIFFS.open(CHANNELS_FILE, FILE_READ);
        if (!in) ok = false;
        uint8_t count = 0;
        if (ok && in.read(&count, 1) != 1) { in.close(); ok = false; }

        File out;
        if (ok) {
          out = SPIFFS.open(String(CHANNELS_FILE) + ".tmp", FILE_WRITE);
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
          SPIFFS.remove(CHANNELS_FILE);
          SPIFFS.rename(String(CHANNELS_FILE) + ".tmp", CHANNELS_FILE);
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
    rebuild_contacts_file_excluding(c->id.pub_key);
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
    rebuild_contacts_file_excluding(c->id.pub_key);
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
    for (uint32_t i=0; i<(uint32_t)getNumContacts(); i++) {
      if (!getContactByIdx(i, c)) continue;
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
  }

  void onContactVisit(const ContactInfo&) override {}
};

static UIMesh* g_uimesh = nullptr;

// ============================================================
// setup()
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(50);

  // 30-second watchdog
  esp_task_wdt_init(30, true);
  esp_task_wdt_add(NULL);

  init_display_and_ui();
  speaker_init();

  if (ui_repeaterscreen)
    lv_obj_add_event_cb(ui_repeaterscreen, cb_repeater_screen_loaded,
                        LV_EVENT_SCREEN_LOADED, nullptr);

  lv_timer_handler();
  if (ui_chatpanel) g_chatpanel_orig_y = lv_obj_get_y(ui_chatpanel);

  g_rtc_ok = g_rtc.begin(&Wire);
  if (g_rtc_ok) serialmon_append("RTC: begin OK (0x51)");
  else          serialmon_append("RTC: begin FAILED (0x51)");

  load_ui_prefs_nvs();
  if (ui_screentimeout) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)g_screen_timeout_s);
    lv_textarea_set_text(ui_screentimeout, buf);
  }

  board.begin();
  if (!radio_init()) {
    serialmon_append("Radio init failed");
    while (1) delay(1000);
  }

  StdRNG& rng = fast_rng;
  rng.begin(radio_get_rng_seed());

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
  SPIFFS.begin(true);

  g_uimesh = new UIMesh(radio_driver, rng, rtc_clock, tables);
  g_mesh = g_uimesh;
  g_uimesh->begin(SPIFFS);
#else
  #error "need to define filesystem"
#endif

  // Restore time from RTC coin battery (stores UTC; WiFi NTP will update when online)
  if (g_rtc_ok) {
    DateTime nowdt = g_rtc.now();
    if (nowdt.year() >= 2025 && nowdt.year() <= 2035) {
      uint32_t utc_epoch = (uint32_t)nowdt.unixtime();
      mesh_set_time_epoch(utc_epoch);
      tz_update_offset_now();  // recalculate DST with actual UTC time
      update_timelabel();
      serialmon_append("Time set from RTC");
    } else {
      serialmon_append("RTC year out of range - waiting for NTP");
    }
  }

  radio_set_params(g_uimesh->getFreqPref(), (float)LORA_BW, (uint8_t)LORA_SF, (uint8_t)LORA_CR);
  radio_set_tx_power(g_uimesh->getTxPowerPref());

  if (ui_devicenamelabel) lv_label_set_text(ui_devicenamelabel, g_uimesh->getNodeName());

  ui_refresh_targets();
  ui_populate_presets();
  ui_populate_timezone_dropdown();
  reg(ui_timezonedropdown, cb_timezone, LV_EVENT_VALUE_CHANGED);

  {
    int saved_idx = load_preset_idx_nvs();
    if (saved_idx >= 0)
      g_uimesh->applyPresetByIdx(saved_idx, false);
  }

  if (ui_textsendtype) {
    lv_textarea_set_max_length(ui_textsendtype, TEXTSEND_MAX_CHARS);
  }

  reg(ui_textsendtype,    cb_textsend_ready, LV_EVENT_READY);
  reg(ui_renamebox,       cb_rename_ready,   LV_EVENT_READY);
  reg(ui_renamebox,       cb_rename_ready,   LV_EVENT_CANCEL);
  reg(ui_screentimeout,   cb_timeout_ready,  LV_EVENT_READY);
  reg(ui_hashtagchannel,  cb_hashtag_ready,  LV_EVENT_READY);
  reg(ui_searchfield,     cb_searchfield_ready, LV_EVENT_READY);

  reg(ui_textsendtype,    cb_textsend_focused, LV_EVENT_FOCUSED);
  reg(ui_textsendtype,    cb_textsend_focused, LV_EVENT_CLICKED);
  reg(ui_renamebox,       cb_rename_focused,   LV_EVENT_FOCUSED);
  reg(ui_screentimeout,   cb_timeout_focused,  LV_EVENT_FOCUSED);
  reg(ui_txpowerslider,   cb_txpower_focused,  LV_EVENT_FOCUSED);
  reg(ui_hashtagchannel,  cb_hashtag_focused,  LV_EVENT_FOCUSED);
  reg(ui_searchfield,     cb_searchfield_focused, LV_EVENT_FOCUSED);
  reg(ui_searchfield,     cb_searchfield_focused, LV_EVENT_CLICKED);

  reg(ui_textsendtype,    cb_textsend_defocused,     LV_EVENT_DEFOCUSED);
  reg(ui_renamebox,       cb_rename_defocused,       LV_EVENT_DEFOCUSED);
  reg(ui_screentimeout,   cb_timeout_defocused,      LV_EVENT_DEFOCUSED);
  reg(ui_txpowerslider,   cb_txpower_defocused,      LV_EVENT_DEFOCUSED);
  reg(ui_hashtagchannel,  cb_hashtag_defocused,      LV_EVENT_DEFOCUSED);
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

  reg(ui_notificationstoggle, cb_notifications_toggle);
  ui_apply_notifications_state();

  reg(ui_autocontacttoggle,  cb_auto_contact_toggle);
  reg(ui_autorepeatertoggle, cb_auto_repeater_toggle);

  // Translation wiring is in features_cb.cpp (Web Apps screen)

  // WiFi subsystem init (UI wiring is in features_cb.cpp)
  wifi_init();

  // Features & Bridges
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
}

// ============================================================
// loop()
// ============================================================

void loop() {
  esp_task_wdt_reset();

  if (g_confirm_action != ConfirmAction::NONE) {
    if ((int32_t)(millis() - g_confirm_deadline_ms) > 0) {
      if (g_confirm_action == ConfirmAction::PURGE_DATA) purge_btn_restore_position();
      confirm_clear();
    }
  }

  {
    static uint32_t last_time_update_ms = 0;
    uint32_t now_ms = millis();
    if (now_ms - last_time_update_ms >= 60000UL) {
      last_time_update_ms = now_ms;
      update_timelabel();
    }
  }

  uint32_t now = millis();
  uint32_t timeout_ms = g_screen_timeout_s * 1000UL;

  if (g_screen_awake) {
    if (timeout_ms >= 5000UL && (now - g_last_touch_ms) > timeout_ms) {
      screen_sleep();
    }
  } else {
    uint16_t x, y;
    if (gfx.getTouch(&x, &y)) {
      g_swallow_touch   = true;
      g_touch_was_press = false;
      note_touch_activity();
    }
  }

  // ---- Soft wake-up ramp tick ----
  if (g_ramp_current > 0 && g_ramp_current < g_ramp_target &&
      (int32_t)(now - g_ramp_next_ms) >= 0) {
    g_ramp_current++;
    if (g_ramp_current > g_ramp_target) g_ramp_current = g_ramp_target;
    i2c_cmd(g_ramp_current);
    g_ramp_next_ms = now + RAMP_STEP_MS;
    if (g_ramp_current >= g_ramp_target) g_ramp_current = 0;
  }

  // ---- Drain all deferred LVGL operations before lv_timer_handler() ----

  // 1. Incoming chat messages
  if (g_deferred_msg_count > 0) {
    for (int i = 0; i < g_deferred_msg_count; i++) {
      DeferredChatMsg& msg = g_deferred_msgs[i];
      lv_obj_t* status_label = chat_add(msg.out, msg.txt, msg.live_status, 0,
                                        msg.sig[0] ? msg.sig : nullptr);
      if (msg.out && msg.live_status && msg.msg_ts && g_uimesh) {
        g_uimesh->bindPmStatusLabel(msg.msg_ts, status_label);
      }
    }
    g_deferred_msg_count = 0;
  }
  if (g_deferred_msg_dropped > 0) {
    char dbuf[48];
    snprintf(dbuf, sizeof(dbuf), "(%d messages missed - queue full)", g_deferred_msg_dropped);
    chat_add(false, dbuf);
    g_deferred_msg_dropped = 0;
  }

  // 1b. Deferred sound
  if (g_deferred_sound) {
    uint8_t snd = g_deferred_sound;
    g_deferred_sound = 0;
    if (snd == 1) beep_msg_in();
    else if (snd == 2) beep_msg_out();
    else if (snd == 3) beep_error();
  }

  // 2. Refresh targets
  if (g_deferred_refresh_targets) {
    g_deferred_refresh_targets = false;
    ui_refresh_targets();
  }

  // 3. Serial monitor label
  if (g_deferred_serialmon_dirty && serialLabel) {
    g_deferred_serialmon_dirty = false;
    memcpy(g_serial_buf_front, g_serial_buf, g_serial_len + 1);
    lv_label_set_text(serialLabel, g_serial_buf_front);
    lv_obj_scroll_to_y(ui_serialmonitorwindow, LV_COORD_MAX, LV_ANIM_OFF);
  }

  // 3b. Deferred time label
  if (g_deferred_timelabel_dirty) {
    g_deferred_timelabel_dirty = false;
    update_timelabel();
  }
  if (g_deferred_route_label_dirty) {
    g_deferred_route_label_dirty = false;
    chat_update_route_label();
  }
  if (g_deferred_wifi_status_dirty) {
    g_deferred_wifi_status_dirty = false;
    wifi_ui_update_status();
  }
  if (g_deferred_features_dirty) {
    g_deferred_features_dirty = false;
    features_update_status_labels();
  }
  if (g_deferred_discover_done) {
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
    if (s.file_dirty && s.chat_key[0]) {
      esp_task_wdt_reset();
      update_tx_status_by_msg_ts(String(s.chat_key), s.msg_ts, s.state);
      s.file_dirty = false;
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
      chat_scroll_to_newest();
    }
  }

  // 7. Deferred scroll-to-bottom
  if (g_deferred_scroll_bottom) {
    g_deferred_scroll_bottom = false;
    lv_obj_update_layout(lv_scr_act());
    chat_scroll_to_newest();
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
  lv_timer_handler();

  // DMA fence
  gfx.waitDMA();

  if (g_mesh) g_mesh->loop();

  if (g_uimesh) g_uimesh->checkPmRetryTimeout();
  if (g_uimesh) g_uimesh->checkPmHardTimeout();
  if (g_uimesh) g_uimesh->checkPmUnconfirmedExpiry();

  // Deferred radio actions
  if (g_deferred_flood_advert) { g_deferred_flood_advert = false; if (g_uimesh) g_uimesh->sendFloodAdvert(0); }
  if (g_deferred_zero_advert)  { g_deferred_zero_advert = false;  if (g_uimesh) g_uimesh->sendZeroHopAdvert(0); }
  if (g_deferred_preset_idx >= 0) { int idx = g_deferred_preset_idx; g_deferred_preset_idx = -1; if (g_uimesh) g_uimesh->applyPresetByIdx(idx, false); }

  rtc_clock.tick();
  wifi_loop();
  ota_loop();
  webdash_loop();
  tgbridge_loop();
  translate_loop();
  poll_channel_receipt_if_due();

  if (g_del.kind != TargetKind::NONE &&
      (millis() - g_del.armed_ms) > 2000) {
    // del_warn_clear is in home_ui — but we need the inline version here
    // since home_ui.cpp's del_warn_clear calls build_homescreen_list
    if (ui_searchfield) {
      lv_obj_set_style_text_color(ui_searchfield, lv_color_hex(g_theme->search_text), 0);
      lv_textarea_set_text(ui_searchfield, g_search_filter);
    }
    g_del = {};
    g_long_press_just_fired = false;
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

  if (g_notifications_dirty) {
    g_notifications_dirty = false;
    save_notifications_nvs();
  }

  // Debounced contact save
  if (g_contacts_save_dirty && (millis() - g_contacts_save_ms) > 5000) {
    g_contacts_save_dirty = false;
    esp_task_wdt_reset();
    if (g_uimesh) g_uimesh->saveContactsNow();
  }

  if (g_login_timeout_ms && (int32_t)(millis() - g_login_timeout_ms) > 0) {
    g_login_timeout_ms = 0;
    if (!g_repeater_logged_in && g_selected_repeater && g_mesh && g_login_retry_count < 2) {
      g_login_retry_count++;
      uint32_t est_timeout;
      int result = g_mesh->sendLogin(*g_selected_repeater, g_login_last_pw, est_timeout);
      if (result != MSG_SEND_FAILED) {
        memcpy(g_login_pending_key, g_selected_repeater->id.pub_key, 4);
        g_login_timeout_ms = millis() + max(est_timeout * 4, (uint32_t)20000);
        char msg[48];
        snprintf(msg, sizeof(msg), "Retry %d/2 - waiting for response...", g_login_retry_count);
        repeater_update_monitor(msg);
        return;
      }
    }
    memset(g_login_pending_key, 0, 4);
    if (!g_repeater_logged_in)
      repeater_update_monitor("No response - tap Login to retry.");
  }

  // ---- Frame rate limiter ----
  delay(1);
}

// ============================================================
// mesh_api wrappers — let extracted modules call UIMesh methods
// ============================================================

void mesh_set_tx_power_pref(int8_t dbm)               { if (g_uimesh) g_uimesh->setTxPowerPref(dbm); }
void mesh_send_zero_hop_advert(int delay_ms)           { if (g_uimesh) g_uimesh->sendZeroHopAdvert(delay_ms); }
uint32_t mesh_send_discover_repeaters()                { return g_uimesh ? g_uimesh->sendDiscoverRepeaters() : 0; }
void mesh_send_flood_advert(int delay_ms)              { if (g_uimesh) g_uimesh->sendFloodAdvert(delay_ms); }
void mesh_rename_if_non_empty(const char* name)        { if (g_uimesh) g_uimesh->renameIfNonEmpty(name); }
int  mesh_join_hashtag_channel(const String& raw)      { return g_uimesh ? g_uimesh->joinHashtagChannel(raw) : -1; }
void mesh_purge_contacts_and_repeaters()               { if (g_uimesh) g_uimesh->purgeContactsAndRepeaters(); }
int  mesh_current_channel_idx()                        { return g_uimesh ? g_uimesh->currentChannelIdx() : -1; }
bool mesh_select_contact_by_pubkey(const uint8_t* pk)  { return g_uimesh ? g_uimesh->selectContactByPubKey(pk) : false; }
bool mesh_select_channel_by_idx(int idx)               { return g_uimesh ? g_uimesh->selectChannelByIdx(idx) : false; }
bool mesh_delete_contact_by_pubkey(const uint8_t* pk)  { return g_uimesh ? g_uimesh->deleteContactByPubKey(pk) : false; }
bool mesh_delete_repeater_by_pubkey(const uint8_t* pk) { return g_uimesh ? g_uimesh->deleteRepeaterByPubKey(pk) : false; }
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
  return g_uimesh ? g_uimesh->getNodeName() : "CrowPanel";
}
void mesh_get_fixed_position(double* lat, double* lon) {
  if (lat) *lat = g_uimesh ? g_uimesh->getNodeLat() : 0.0;
  if (lon) *lon = g_uimesh ? g_uimesh->getNodeLon() : 0.0;
}
void mesh_set_fixed_position(double lat, double lon) {
  if (!g_uimesh) return;
  g_uimesh->setNodePosition(lat, lon);
  g_deferred_flood_advert = true;
}
void mesh_clear_fixed_position() {
  if (!g_uimesh) return;
  g_uimesh->clearNodePosition();
  g_deferred_flood_advert = true;
}

void mesh_populate_repeater_list() {
  if (!g_uimesh) return;
  g_repeater_count = 0;
  ContactsIterator it;
  ContactInfo c;
  while (it.hasNext(g_uimesh, c)) {
    if (c.type == ADV_TYPE_REPEATER && g_repeater_count < MAX_DD_ENTRIES) {
      g_repeater_list[g_repeater_count++] = g_uimesh->lookupContactByPubKey(c.id.pub_key, 32);
    }
  }
}

int mesh_repeater_login(const uint8_t* pub_key, const char* password) {
  if (!g_uimesh) return -1;
  ContactInfo* c = g_uimesh->lookupContactByPubKey(pub_key, 32);
  if (!c) return -1;
  uint32_t est_timeout;
  int result = g_uimesh->sendLogin(*c, password, est_timeout);
  if (result == MSG_SEND_FAILED) return -1;
  g_selected_repeater = c;
  memcpy(g_login_pending_key, pub_key, 4);
  g_login_retry_count = 0;
  g_login_timeout_ms = millis() + max(est_timeout * 4, (uint32_t)20000);
  strncpy(g_login_last_pw, password, sizeof(g_login_last_pw) - 1);
  g_login_last_pw[sizeof(g_login_last_pw) - 1] = '\0';
  repeater_update_monitor("Login sent, waiting for response...");
  return 0;
}

int mesh_repeater_request_status(const uint8_t* pub_key) {
  if (!g_uimesh || !g_repeater_logged_in || !g_selected_repeater) return -1;
  if (memcmp(g_selected_repeater->id.pub_key, pub_key, 4) != 0) return -1;
  uint32_t tag, est_timeout;
  g_uimesh->sendRequest(*g_selected_repeater, (uint8_t)0x01, tag, est_timeout);
  g_status_pending_key = tag;
  repeater_update_monitor("Refreshing status...");
  return 0;
}

int mesh_repeater_request_neighbours(const uint8_t* pub_key) {
  if (!g_uimesh || !g_repeater_logged_in || !g_selected_repeater) return -1;
  if (memcmp(g_selected_repeater->id.pub_key, pub_key, 4) != 0) return -1;
  uint8_t req[11] = {0x06, 0, 20, 0, 0, 0, 4, 0, 0, 0, 0};
  uint32_t tag, est_timeout;
  int result = g_uimesh->sendRequest(*g_selected_repeater, req, sizeof(req), tag, est_timeout);
  if (result == MSG_SEND_FAILED) return -1;
  g_neighbours_pending_key = tag;
  repeater_update_monitor("Neighbours request sent, waiting...");
  return 0;
}

int mesh_repeater_send_advert(const uint8_t* pub_key) {
  if (!g_uimesh || !g_repeater_logged_in || !g_selected_repeater) return -1;
  if (memcmp(g_selected_repeater->id.pub_key, pub_key, 4) != 0) return -1;
  uint32_t est_timeout;
  g_uimesh->sendCommandData(*g_selected_repeater,
    g_uimesh->getRTCClock()->getCurrentTime(), 0, "advert", est_timeout);
  repeater_update_monitor("Advert command sent.");
  return 0;
}

int mesh_repeater_send_reboot(const uint8_t* pub_key) {
  if (!g_uimesh || !g_repeater_logged_in || !g_selected_repeater) return -1;
  if (memcmp(g_selected_repeater->id.pub_key, pub_key, 4) != 0) return -1;
  uint32_t est_timeout;
  g_uimesh->sendCommandData(*g_selected_repeater,
    g_uimesh->getRTCClock()->getCurrentTime(), 0, "reboot", est_timeout);
  repeater_update_monitor("Reboot command sent.");
  return 0;
}
