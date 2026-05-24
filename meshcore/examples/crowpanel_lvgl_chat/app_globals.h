#pragma once
// ============================================================
// app_globals.h — Shared declarations for the split modules
// ============================================================

// FIRMWARE_VERSION is defined in platformio.ini build flags
#define SERIALMON_VERBOSE 0

#include <Arduino.h>
#include <lvgl.h>
#include <Mesh.h>

#if defined(ESP32)
  #include <SPIFFS.h>
  #include <Preferences.h>
#endif

#include <helpers/BaseChatMesh.h>
#include <RTClib.h>
#include <target.h>

// ---- Shared enums & structs ----
enum class TargetKind : uint8_t { NONE=0, CHANNEL=1, CONTACT=2 };

struct DeletePending {
  TargetKind kind        = TargetKind::NONE;
  int        ch_idx      = -1;
  uint8_t    pub_key[32] = {};
  uint32_t   armed_ms    = 0;
};

struct UITheme {
  uint32_t screen_bg;
  uint32_t bubble_out, bubble_in, bubble_text, bubble_ts, bubble_name;
  uint32_t status_delivered, status_failed, status_pending, signal_text;
  uint32_t chat_header_bg, chat_header_text;
  uint32_t hs_contact_bg, hs_contact_border, hs_contact_name, hs_contact_seen;
  uint32_t hs_channel_bg, hs_channel_border, hs_channel_text, hs_channel_icon, hs_hashtag_icon;
  uint32_t hs_badge_bg;
  uint32_t scroll_btn_bg, btn_active, btn_danger, search_text, search_warn;
  uint32_t new_divider;
  uint32_t signal_strong, signal_mid, signal_weak, signal_none;
};

struct NodePrefs {
  float airtime_factor;
  char node_name[32];
  double node_lat, node_lon;
  float freq;
  int8_t tx_power_dbm;
  uint8_t path_hash_mode;  // 0=1-byte, 1=2-byte, 2=3-byte path hashes
};

struct DeferredChatMsg {
  bool out;
  bool live_status;
  uint32_t msg_ts;
  char txt[256];
  char sig[32];
};

struct BubbleTapData {
  TargetKind kind;
  int        channel_idx;
  uint8_t    pub_key[32];
};

struct DropdownEntry {
  TargetKind kind;
  int channel_idx;
  mesh::Identity contact_id;
  char name[32];
  uint32_t last_advert_ts;
};

struct ChannelReceiptPoll {
  bool      active = false;
  uint16_t  heard_count = 0;
  lv_obj_t* status_label = nullptr;
  String    chat_key;
  uint32_t  discover_tag = 0;
  uint32_t  timeout_ms = 0;
  uint8_t   seen_keys[16][4];   // first 4 bytes of each unique responder
  int       seen_count = 0;
};

struct RepeaterStats {
  uint16_t batt_milli_volts;
  uint16_t curr_tx_queue_len;
  int16_t  noise_floor;
  int16_t  last_rssi;
  uint32_t n_packets_recv;
  uint32_t n_packets_sent;
  uint32_t total_air_time_secs;
  uint32_t total_up_time_secs;
  uint32_t n_sent_flood, n_sent_direct;
  uint32_t n_recv_flood, n_recv_direct;
  uint16_t err_events;
  int16_t  last_snr;
  uint16_t n_direct_dups, n_flood_dups;
  uint32_t total_rx_air_time_secs;
  uint32_t n_recv_errors;
};

// ---- Preset struct (used by UIMesh::presets) ----
struct Preset { const char* name; float freq; float bw; uint8_t sf; uint8_t cr; int8_t tx; };

// ---- Constants ----
static const int DEFERRED_MSG_MAX = 32;
static const int MAX_DD_ENTRIES = 64;
static const int MAX_UNREAD_SLOTS = 64;
static const uint8_t CONTACT_FLAG_HIDDEN = 0x80;
static const uint8_t REQ_TYPE_GET_PKT_RECEIPT = 0x08;

#define SEND_TIMEOUT_BASE_MILLIS          4000
#define FLOOD_SEND_TIMEOUT_FACTOR         20.0f
#define DIRECT_SEND_PERHOP_FACTOR         6.0f
#define DIRECT_SEND_PERHOP_EXTRA_MILLIS   250
#define TEXTSEND_MAX_CHARS                150
#define PUBLIC_GROUP_PSK  "izOH6cXN6mrJ5e26oRXNcg=="

// Layout constants are declared in ui_theme.h (C linkage)
// g_landscape_mode is the only layout global here (C++ only)
extern bool    g_landscape_mode;

// Backlight constants
static const uint8_t BL_OFF         = 0x05;
static const uint8_t BL_MIN_VISIBLE = 0x06;
static const uint8_t BL_MAX         = 0x10;

// Speaker
#define SPK_PIN  0
#define SPK_LEDC_CHANNEL 0

// ---- Extern globals (defined in main.cpp) ----
extern BaseChatMesh* g_mesh;
extern const UITheme THEME_DARK;
extern const UITheme* g_theme;

// Display / LVGL widget pointers
extern lv_obj_t *serialLabel;
extern lv_obj_t *g_pending_status_label;  // channel receipts only
extern lv_obj_t *g_scroll_btn;
extern lv_obj_t *g_chat_header_label;
extern lv_obj_t *g_chat_route_label;
extern char      g_chat_target_name[64];
extern bool      g_in_chat_mode;
extern char      g_search_filter[64];
extern lv_coord_t g_chatpanel_orig_y;
extern bool      g_loading_history;
extern bool      g_notifications_dirty;

// Delete state
extern DeletePending g_del;
extern bool          g_long_press_just_fired;

// Deferred state
extern char   g_deferred_send_text[241];
extern bool   g_deferred_send_pending;
extern bool   g_deferred_scroll_bottom;
extern bool   g_deferred_flood_advert;
extern bool   g_deferred_zero_advert;
extern int    g_deferred_preset_idx;
extern bool   g_contacts_save_dirty;
extern uint32_t g_contacts_save_ms;
extern char   g_deferred_status_char;
extern uint8_t g_deferred_sound;
extern bool   g_deferred_refresh_targets;
extern bool   g_deferred_serialmon_dirty;
extern bool   g_deferred_timelabel_dirty;
extern bool   g_deferred_route_label_dirty;
extern bool   g_deferred_wifi_status_dirty;
extern bool   g_deferred_receipt_update;
extern char   g_deferred_repeater_mon[640];
extern bool   g_deferred_repeater_mon_dirty;
extern int8_t g_deferred_repeater_btns;
extern DeferredChatMsg g_deferred_msgs[];
extern int g_deferred_msg_count;
extern int g_deferred_msg_dropped;
extern bool   g_deferred_swipe_back;
extern bool   g_deferred_swipe_home;

// Screen state
extern volatile bool     g_screen_awake;
extern volatile uint32_t g_last_touch_ms;
extern uint8_t           g_backlight_level;
extern bool              g_swallow_touch;
extern bool              g_touch_was_press;
extern bool              g_dismiss_keyboard;
extern uint32_t          g_screen_timeout_s;

// Feature flags
extern bool g_notifications_enabled;
extern bool g_auto_contact_enabled;
extern bool g_auto_repeater_enabled;
extern bool g_packet_forward_enabled;
extern bool g_position_advert_enabled;
extern bool g_auto_translate_enabled;
extern int  g_translate_lang_idx;
extern bool g_manual_discover_active;
extern uint32_t g_discover_tag;
extern bool g_deferred_discover_done;

// Discover response tracking (for listing results at scan-done)
#define MAX_DISCOVER_RESULTS 16
extern char g_discover_names[MAX_DISCOVER_RESULTS][32];
extern int  g_discover_result_count;
extern bool g_purged_this_session;
extern bool g_speaker_enabled;

// Notification tracking
struct ContactUnread {
  uint8_t pub_key[32];
  uint16_t count;
  bool valid;
};
struct ChannelUnread {
  int channel_idx;
  uint16_t count;
  bool valid;
};
extern ContactUnread g_contact_unread[MAX_UNREAD_SLOTS];
extern ChannelUnread g_channel_unread[MAX_UNREAD_SLOTS];

// SNR tracking
struct ContactSNR {
  uint8_t pub_key[32];
  int8_t  last_snr;
  bool    valid;
};
extern ContactSNR g_contact_snr[MAX_UNREAD_SLOTS];

// Chat route indicator state
extern uint8_t  g_chat_route_path_len;
extern bool     g_chat_route_is_contact;

// RTC / Time
extern RTC_PCF8563 g_rtc;
extern bool g_rtc_ok;
extern int32_t DISPLAY_UTC_OFFSET_S;

// Ramp state
extern uint8_t  g_ramp_target;
extern uint8_t  g_ramp_current;
extern uint32_t g_ramp_next_ms;
static const uint32_t RAMP_STEP_MS = 180;

// Swipe tracking
extern uint16_t g_swipe_start_x;
extern uint16_t g_swipe_start_y;
extern bool     g_swipe_tracking;

// Dropdown data
extern DropdownEntry dd_channels[MAX_DD_ENTRIES];
extern int           dd_channels_count;
extern DropdownEntry dd_contacts[MAX_DD_ENTRIES];
extern int           dd_contacts_count;
extern char dd_opts_buf[MAX_DD_ENTRIES * 41];

// Channel mask
extern uint32_t g_deleted_channel_mask;
extern uint32_t g_muted_channel_mask;

// Homescreen bubble pool
extern BubbleTapData g_bubble_pool[MAX_DD_ENTRIES * 2];
extern int           g_bubble_pool_count;

// Channel receipt poll
extern ChannelReceiptPoll g_channel_receipt;

// Repeater
extern bool         g_repeater_logged_in;
extern uint8_t      g_login_pending_key[4];
extern uint32_t     g_login_timeout_ms;
extern uint8_t      g_login_retry_count;
extern char         g_login_last_pw[16];
extern uint32_t     g_status_pending_key;
extern uint32_t     g_neighbours_pending_key;
extern ContactInfo* g_selected_repeater;
extern ContactInfo* g_repeater_list[MAX_DD_ENTRIES];
extern int          g_repeater_count;

// Serial monitor buffers
extern const size_t SERIAL_BUF_SIZE;
extern const size_t SERIAL_BUF_TRIM;
extern char  g_serial_buf[];
extern char  g_serial_buf_front[];
extern size_t g_serial_len;

// Speaker btn
extern lv_obj_t* g_speaker_btn;
extern lv_obj_t* g_discover_repeaters_btn;
extern lv_obj_t* g_pkt_fwd_btn;

// Keyboard
extern bool g_kb_greek;

// Confirm action
enum class ConfirmAction : uint8_t { NONE=0, PURGE_DATA=4 };
extern ConfirmAction g_confirm_action;
extern uint32_t g_confirm_deadline_ms;

// Settings form state

// Repeater screen
extern bool g_repeater_cbs_wired;
extern char g_repeater_filter[32];

// Per-message outbound PM tracking (ring buffer)
#define PM_RING_SIZE       8
#define MAX_PM_ACK_CODES   3   // original + 2 retries
#define PM_LATE_EXPIRY_MS  120000  // keep failed records 2 min for late ACK
#define PM_UNCONFIRMED_MS  60000   // show "Unknown, acknowledge will fail in 1'" for 1 min before "Failed"

struct OutboundPM {
  bool     active;
  bool     ui_dirty;       // label needs update in main loop
  bool     file_dirty;     // file needs update in main loop
  uint32_t msg_ts;         // message timestamp = unique message ID
  char     chat_key[20];   // "ct_XXXXXXXXXXXX"
  uint32_t ack_codes[MAX_PM_ACK_CODES];
  uint8_t  ack_count;
  uint8_t  retry_count;
  lv_obj_t* status_label;  // LVGL label for this bubble (nullptr if off-screen)
  char     state;          // 'P'=pending, 'N'=failed, 'D'=delivered
  uint32_t hard_timeout_ms;
  uint32_t retry_timeout_ms; // per-retry ACK timeout (0 = not waiting)
  uint32_t expiry_ms;      // eviction time (0 while pending)
  ContactInfo* recipient;
  char     retry_text[241];
  bool     resend_offered;   // true after resend button has been shown
};
extern OutboundPM g_pm_ring[PM_RING_SIZE];

// ---- Features (Telegram bridge, Web dashboard, OTA) ----
extern bool     g_tgbridge_enabled;
extern bool     g_webdash_enabled;
extern bool     g_deferred_features_dirty;
