// ============================================================
// settings_cb.cpp — Settings callbacks, toggles, timezone, presets
// ============================================================

#include "settings_cb.h"
#include "app_globals.h"
#include "utils.h"
#include "persistence.h"
#include "chat_ui.h"
#include "home_ui.h"
#include "display.h"
#include "mesh_api.h"
#include "repeater_ui.h"
#include "wifi_ntp.h"

#include <lvgl.h>
#include <WiFi.h>

// SquareLine UI widget externs
#include "ui.h"
#include "ui_homescreen.h"
#include "ui_settingscreen.h"
#include "features_ui.h"
#include "features_cb.h"
#include "translate.h"

// ---- Event registration helper ----
void reg(lv_obj_t* o, lv_event_cb_t cb, lv_event_code_t ev) {
  if (o) lv_obj_add_event_cb(o, cb, ev, nullptr);
}
void btn_lbl(lv_obj_t* btn, const char* txt) {
  if (!btn) return;
  lv_obj_t* lbl = lv_obj_get_child(btn, 0);
  if (lbl) lv_label_set_text(lbl, txt);
}

// ---- Settings callbacks ----
void cb_brightness(lv_event_t*) {
  if (!ui_brightnessslider) return;

  int v    = lv_slider_get_value(ui_brightnessslider);
  int vmax = lv_slider_get_max_value(ui_brightnessslider);
  if (vmax <= 0) vmax = 100;
  if (v > vmax)  vmax = v;

  uint8_t bl = (uint8_t)(BL_MIN_VISIBLE + (long)(BL_MAX - BL_MIN_VISIBLE) * (long)v / (long)vmax);
  if (bl < BL_MIN_VISIBLE) bl = BL_MIN_VISIBLE;
  if (bl > BL_MAX) bl = BL_MAX;

  g_backlight_level = bl;
  if (g_screen_awake) i2c_cmd(bl);
}

void cb_txpower_ready(lv_event_t*) {
  if (!ui_txpowerslider || !g_mesh) return;
  const char* txt = lv_textarea_get_text(ui_txpowerslider);
  if (!txt || !*txt) return;
  int val = atoi(txt);
  if (val < 1) val = 1;
  if (val > 22) val = 22;
  int8_t dbm = (int8_t)val;
  radio_set_tx_power(dbm);
  mesh_set_tx_power_pref(dbm);
  // Persist to NVS
  {
    Preferences p;
    p.begin("ui", false);
    p.putChar("tx_power", dbm);
    p.end();
  }
  char buf[8];
  snprintf(buf, sizeof(buf), "%d", (int)dbm);
  lv_textarea_set_text(ui_txpowerslider, buf);
  char line[32];
  snprintf(line, sizeof(line), "TX power: %d dBm (saved)", (int)dbm);
  serialmon_append(line);
  lv_obj_clear_state(ui_txpowerslider, LV_STATE_FOCUSED);
}

void cb_fadvert(lv_event_t*)   { g_deferred_flood_advert = true; }
void cb_zeroadvert(lv_event_t*){ g_deferred_zero_advert = true; }

// ---- Toggle callbacks (settings) ----
void ui_apply_notifications_state() {
  if (ui_notificationstoggle)
    lv_obj_set_style_bg_color(ui_notificationstoggle,
      g_notifications_enabled ? lv_color_hex(g_theme->btn_active) : lv_color_hex(g_theme->btn_danger), 0);
}
void cb_notifications_toggle(lv_event_t*) {
  g_notifications_enabled = !g_notifications_enabled;
  ui_apply_notifications_state();
  serialmon_append(g_notifications_enabled ? "Notifications: ON" : "Notifications: OFF");
}

void ui_apply_auto_contact_state() {
  if (ui_autocontacttoggle)
    lv_obj_set_style_bg_color(ui_autocontacttoggle,
      g_auto_contact_enabled ? lv_color_hex(g_theme->btn_active) : lv_color_hex(g_theme->btn_danger), 0);
}
void cb_auto_contact_toggle(lv_event_t*) {
  g_auto_contact_enabled = !g_auto_contact_enabled;
  ui_apply_auto_contact_state();
  serialmon_append(g_auto_contact_enabled ? "Auto-contact discovery: ON" : "Auto-contact discovery: OFF");
  save_ui_prefs_nvs();
}

void ui_apply_auto_repeater_state() {
  if (ui_autorepeatertoggle)
    lv_obj_set_style_bg_color(ui_autorepeatertoggle,
      g_auto_repeater_enabled ? lv_color_hex(g_theme->btn_active) : lv_color_hex(g_theme->btn_danger), 0);
}
void cb_auto_repeater_toggle(lv_event_t*) {
  g_auto_repeater_enabled = !g_auto_repeater_enabled;
  ui_apply_auto_repeater_state();
  serialmon_append(g_auto_repeater_enabled ? "Auto-repeater discovery: ON" : "Auto-repeater discovery: OFF");
  save_ui_prefs_nvs();
}

// ---- Packet forward toggle ----
void ui_apply_packet_forward_state() {
  if (g_pkt_fwd_btn)
    lv_obj_set_style_bg_color(g_pkt_fwd_btn,
      g_packet_forward_enabled ? lv_color_hex(g_theme->btn_active) : lv_color_hex(g_theme->btn_danger), 0);
}
void cb_packet_forward_toggle(lv_event_t*) {
  g_packet_forward_enabled = !g_packet_forward_enabled;
  ui_apply_packet_forward_state();
  serialmon_append(g_packet_forward_enabled ? "Packet forwarding: ON" : "Packet forwarding: OFF");
  save_ui_prefs_nvs();
}

// ---- Position advert toggle ----
void ui_apply_position_advert_state() {
  if (!ui_positionadverttoggle) return;
  if (g_position_advert_enabled) lv_obj_add_state(ui_positionadverttoggle, LV_STATE_CHECKED);
  else lv_obj_clear_state(ui_positionadverttoggle, LV_STATE_CHECKED);
  if (ui_positionadvert_lbl) {
    lv_label_set_text(ui_positionadvert_lbl, LV_SYMBOL_GPS " Include position in adverts");
    lv_obj_set_style_text_color(ui_positionadvert_lbl,
      g_position_advert_enabled ? lv_color_hex(TH_TEXT) : lv_color_hex(TH_TEXT3), 0);
  }
}
void cb_position_advert_toggle(lv_event_t*) {
  if (!ui_positionadverttoggle) return;
  g_position_advert_enabled = lv_obj_has_state(ui_positionadverttoggle, LV_STATE_CHECKED);
  ui_apply_position_advert_state();
  save_ui_prefs_nvs();
  serialmon_append(g_position_advert_enabled
    ? "Position in node advert: ON"
    : "Position in node advert: OFF");
}

// ---- Auto-translate toggle ----
void ui_apply_auto_translate_state() {
  if (ui_autotranslate_toggle)
    lv_obj_set_style_bg_color(ui_autotranslate_toggle,
      g_auto_translate_enabled ? lv_color_hex(g_theme->btn_active) : lv_color_hex(g_theme->btn_danger), 0);
}
void cb_auto_translate_toggle(lv_event_t*) {
  g_auto_translate_enabled = !g_auto_translate_enabled;
  ui_apply_auto_translate_state();
  serialmon_append(g_auto_translate_enabled ? "Auto-translate: ON" : "Auto-translate: OFF");
  save_ui_prefs_nvs();
}
void cb_translate_lang_changed(lv_event_t*) {
  if (!ui_translate_lang_dd) return;
  g_translate_lang_idx = (int)lv_dropdown_get_selected(ui_translate_lang_dd);
  char logbuf[48];
  snprintf(logbuf, sizeof(logbuf), "Translate language: %s", translate_lang_code(g_translate_lang_idx));
  serialmon_append(logbuf);
  save_ui_prefs_nvs();
}

// ---- Speaker toggle ----
void ui_apply_speaker_btn_state() {
  if (!g_speaker_btn) return;
  lv_obj_set_style_bg_color(g_speaker_btn,
    g_speaker_enabled ? lv_color_hex(g_theme->btn_active) : lv_color_hex(g_theme->btn_danger), 0);
  lv_obj_t* lbl = lv_obj_get_child(g_speaker_btn, 0);
  if (lbl) lv_label_set_text(lbl, g_speaker_enabled
    ? (LV_SYMBOL_AUDIO " Sound ON")
    : (LV_SYMBOL_MUTE " Sound OFF"));
}

void cb_speaker_toggle(lv_event_t*) {
  g_speaker_enabled = !g_speaker_enabled;
  ui_apply_speaker_btn_state();
  save_ui_prefs_nvs();
  serialmon_append(g_speaker_enabled ? "Speaker: ON" : "Speaker: OFF");
  if (g_speaker_enabled) beep_short(1800, 50);
}

// ---- Discover nearby repeaters ----
static lv_timer_t* s_discover_timer = nullptr;

void cb_discover_repeaters(lv_event_t*) {
  if (!g_mesh) return;

  // Reset discover response tracking
  g_discover_result_count = 0;

  // Send control discovery request — repeaters reply immediately
  uint32_t tag = mesh_send_discover_repeaters();
  if (tag == 0) return;
  g_discover_tag = tag;
  g_manual_discover_active = true;

  serialmon_append("Searching for nearby repeaters");

  if (g_discover_repeaters_btn) {
    lv_obj_add_state(g_discover_repeaters_btn, LV_STATE_DISABLED);
    btn_lbl(g_discover_repeaters_btn, LV_SYMBOL_REFRESH " Scanning...");
  }

  // End scan after 15s
  if (s_discover_timer) lv_timer_del(s_discover_timer);
  s_discover_timer = lv_timer_create([](lv_timer_t* t) {
    g_manual_discover_active = false;
    g_discover_tag = 0;
    repeater_populate_dropdown();

    if (g_discover_repeaters_btn) {
      lv_obj_clear_state(g_discover_repeaters_btn, LV_STATE_DISABLED);
      btn_lbl(g_discover_repeaters_btn, LV_SYMBOL_GPS " Repeater Discovery");
    }

    int n = g_discover_result_count;
    char msg[48];
    snprintf(msg, sizeof(msg), "Scan done: %d repeater%s found", n, n == 1 ? "" : "s");
    serialmon_append(msg);
    for (int i = 0; i < n; i++) {
      char line[64];
      snprintf(line, sizeof(line), "  %d. %s", i + 1, g_discover_names[i]);
      serialmon_append_color(0x00FFC8, line);
    }

    lv_timer_del(t);
    s_discover_timer = nullptr;
  }, 15000, nullptr);
}

// ---- Floor noise measurement ----
void cb_floor_noise(lv_event_t*) {
  // Take several RSSI samples while idle and report min/max/avg
  const int N = 20;
  float sum = 0, mn = 0, mx = -200;
  for (int i = 0; i < N; i++) {
    float r = radio_driver.getCurrentRSSI();
    sum += r;
    if (r < mn || i == 0) mn = r;
    if (r > mx) mx = r;
    delay(5);
  }
  float avg = sum / N;
  char buf[96];
  snprintf(buf, sizeof(buf), "Floor noise: avg %.0f dBm  min %.0f  max %.0f  (%d samples)",
           avg, mn, mx, N);
  serialmon_append(buf);
}

// ---- Orientation toggle ----
void cb_orientation_toggle(lv_event_t*) {
  g_landscape_mode = !g_landscape_mode;
  save_landscape_nvs(g_landscape_mode);
  esp_restart();
}

void ui_apply_orientation_btn_state() {
  if (!ui_orientationtoggle) return;
  btn_lbl(ui_orientationtoggle,
    g_landscape_mode ? LV_SYMBOL_LOOP " Switch to Portrait"
                     : LV_SYMBOL_LOOP " Switch to Landscape");
}

// ---- Preset ----
void cb_preset(lv_event_t*) {
  if (!ui_presetsdropdown || !g_mesh) return;
  g_deferred_preset_idx = (int)lv_dropdown_get_selected(ui_presetsdropdown);
}

// ---- Timezone dropdown (with DST support) ----

// DST rule families
enum DSTRule : uint8_t { TZ_DST_NONE=0, TZ_DST_EU, TZ_DST_US, TZ_DST_AU };

struct TZEntry {
  const char* label;
  int32_t  std_offset_s;  // standard time offset from UTC
  DSTRule  dst_rule;      // which DST rule applies (TZ_DST_NONE = no DST)
};

static const TZEntry TZ_LIST[] = {
  { "UTC-10  (Hawaii)",             -36000, TZ_DST_NONE },
  { "UTC-9   (Alaska)",             -32400, TZ_DST_US   },
  { "UTC-8   (Los Angeles)",        -28800, TZ_DST_US   },
  { "UTC-7   (Denver)",             -25200, TZ_DST_US   },
  { "UTC-6   (Chicago)",            -21600, TZ_DST_US   },
  { "UTC-5   (New York)",           -18000, TZ_DST_US   },
  { "UTC-4   (Halifax)",            -14400, TZ_DST_US   },
  { "UTC-3   (Buenos Aires)",       -10800, TZ_DST_NONE },
  { "UTC-1   (Azores)",              -3600, TZ_DST_EU   },
  { "UTC     (London)",                  0, TZ_DST_EU   },
  { "UTC+1   (Amsterdam, Brussels)", 3600,  TZ_DST_EU   },
  { "UTC+2   (Athens, Helsinki)",    7200,  TZ_DST_EU   },
  { "UTC+2   (Cairo) no DST",       7200,  TZ_DST_NONE },
  { "UTC+3   (Moscow, Istanbul)",   10800,  TZ_DST_NONE },
  { "UTC+4   (Dubai)",              14400,  TZ_DST_NONE },
  { "UTC+5   (Karachi)",            18000,  TZ_DST_NONE },
  { "UTC+5:30 (Mumbai, Delhi)",     19800,  TZ_DST_NONE },
  { "UTC+6   (Dhaka)",              21600,  TZ_DST_NONE },
  { "UTC+7   (Bangkok, Jakarta)",   25200,  TZ_DST_NONE },
  { "UTC+8   (Beijing, Singapore)", 28800,  TZ_DST_NONE },
  { "UTC+9   (Tokyo, Seoul)",       32400,  TZ_DST_NONE },
  { "UTC+9:30 (Adelaide)",          34200,  TZ_DST_AU },
  { "UTC+10  (Sydney)",             36000,  TZ_DST_AU },
  { "UTC+11  (Solomon Islands)",    39600,  TZ_DST_NONE },
  { "UTC+12  (Auckland)",           43200,  TZ_DST_NONE },
};
static const int TZ_COUNT = (int)(sizeof(TZ_LIST) / sizeof(TZ_LIST[0]));

// Compute day-of-week: 0=Sun..6=Sat  (Tomohiko Sakamoto)
static int dow(int y, int m, int d) {
  static const int t[] = {0,3,2,5,0,3,5,1,4,6,2,4};
  if (m < 3) y--;
  return (y + y/4 - y/100 + y/400 + t[m-1] + d) % 7;
}

// Find the last Sunday of a given month/year, returning day-of-month
static int last_sunday(int year, int month) {
  int days_in_month;
  switch (month) {
    case 2:  days_in_month = ((year%4==0 && year%100!=0) || year%400==0) ? 29 : 28; break;
    case 4: case 6: case 9: case 11: days_in_month = 30; break;
    default: days_in_month = 31; break;
  }
  int wd = dow(year, month, days_in_month); // 0=Sun
  return days_in_month - wd;
}

// Find the Nth Sunday of a given month/year (N=1 first, N=2 second)
static int nth_sunday(int year, int month, int n) {
  int wd1 = dow(year, month, 1); // weekday of 1st
  int first_sun = (wd1 == 0) ? 1 : (8 - wd1);
  return first_sun + (n - 1) * 7;
}

// Is DST active for a given UTC epoch?
static bool is_dst_active(uint32_t utc_epoch, DSTRule rule) {
  if (rule == TZ_DST_NONE) return false;

  // Convert epoch to date components
  time_t t = (time_t)utc_epoch;
  struct tm tm;
  gmtime_r(&t, &tm);
  int year = tm.tm_year + 1900;
  int mon  = tm.tm_mon + 1;     // 1-12
  int day  = tm.tm_mday;
  int hour_utc = tm.tm_hour;

  if (rule == TZ_DST_EU) {
    // EU: last Sunday of March 01:00 UTC → last Sunday of October 01:00 UTC
    int mar_sun = last_sunday(year, 3);
    int oct_sun = last_sunday(year, 10);
    // Build month*100+day for easy comparison
    int md = mon * 100 + day;
    int start = 3 * 100 + mar_sun;
    int end   = 10 * 100 + oct_sun;
    if (md > start && md < end) return true;
    if (md < start || md > end) return false;
    if (md == start) return hour_utc >= 1;
    if (md == end)   return hour_utc < 1;
    return false;
  }

  if (rule == TZ_DST_US) {
    // US: 2nd Sunday of March 02:00 local → 1st Sunday of November 02:00 local
    // Approximate with UTC: 2nd Sun Mar 07:00 UTC (for EST) — but varies by zone.
    // Simplify: use local standard time for the transition check.
    // We check against UTC+offset, which is standard time.
    int mar_sun2 = nth_sunday(year, 3, 2);
    int nov_sun1 = nth_sunday(year, 11, 1);
    int md = mon * 100 + day;
    int start = 3 * 100 + mar_sun2;
    int end   = 11 * 100 + nov_sun1;
    if (md > start && md < end) return true;
    if (md < start || md > end) return false;
    // On transition days, approximate: DST starts/ends at ~02:00 local ≈ 07:00-10:00 UTC
    // Since we can't know exact local hour without recursion, use a safe midpoint
    if (md == start) return hour_utc >= 10; // conservative: after 10:00 UTC all US zones are DST
    if (md == end)   return hour_utc < 6;   // conservative: before 06:00 UTC all US zones still DST
    return false;
  }

  if (rule == TZ_DST_AU) {
    // Australia (southern): 1st Sunday of October → 1st Sunday of April (next year)
    // DST is in southern summer: Oct → Apr
    int oct_sun1 = nth_sunday(year, 10, 1);
    int apr_sun1 = nth_sunday(year, 4, 1);
    int md = mon * 100 + day;
    int start = 10 * 100 + oct_sun1;
    int end   = 4 * 100 + apr_sun1;
    // Southern hemisphere: DST from Oct to Apr (wraps around new year)
    if (mon >= 10) {
      if (md > start) return true;
      if (md == start) return hour_utc >= 16; // ~02:00 AEST
      return false;
    }
    if (mon <= 4) {
      if (md < end) return true;
      if (md == end) return hour_utc < 16;
      return false;
    }
    return false; // May-Sep: no DST
  }

  return false;
}

// Get the effective offset (std + DST if active)
int32_t tz_get_effective_offset(int tz_idx, uint32_t utc_epoch) {
  if (tz_idx < 0 || tz_idx >= TZ_COUNT) tz_idx = 10; // default Amsterdam
  int32_t off = TZ_LIST[tz_idx].std_offset_s;
  if (is_dst_active(utc_epoch, TZ_LIST[tz_idx].dst_rule))
    off += 3600; // DST = +1 hour
  return off;
}

// ---- Saved timezone index (stored as index, not raw offset) ----
static int s_tz_index = 10; // default UTC+1 Amsterdam

static int tz_index_from_offset(int32_t offset_s) {
  // Legacy migration: find best match by standard offset
  for (int i = 0; i < TZ_COUNT; i++)
    if (TZ_LIST[i].std_offset_s == offset_s) return i;
  return 10; // default Amsterdam
}

void ui_populate_timezone_dropdown() {
  if (!ui_timezonedropdown) return;
  static char opts[TZ_COUNT * 40];
  opts[0] = '\0';
  for (int i = 0; i < TZ_COUNT; i++) {
    strncat(opts, TZ_LIST[i].label, sizeof(opts) - strlen(opts) - 2);
    if (i < TZ_COUNT - 1) strncat(opts, "\n", sizeof(opts) - strlen(opts) - 1);
  }
  lv_dropdown_set_options(ui_timezonedropdown, opts);
  lv_dropdown_set_selected(ui_timezonedropdown, (uint16_t)s_tz_index);
  lv_obj_set_style_text_font(ui_timezonedropdown, &lv_font_montserrat_16, 0);
  lv_obj_t* list = lv_dropdown_get_list(ui_timezonedropdown);
  if (list) lv_obj_set_style_text_font(list, &lv_font_montserrat_16, 0);
}

void cb_timezone(lv_event_t*) {
  if (!ui_timezonedropdown) return;
  uint16_t sel = lv_dropdown_get_selected(ui_timezonedropdown);
  if (sel >= (uint16_t)TZ_COUNT) return;
  s_tz_index = sel;
  // Compute effective offset with DST
  uint32_t utc_now = rtc_clock.getCurrentTime();
  DISPLAY_UTC_OFFSET_S = tz_get_effective_offset(sel, utc_now);
  save_ui_prefs_nvs();
  update_timelabel();
  char line[64];
  bool dst = is_dst_active(utc_now, TZ_LIST[sel].dst_rule);
  snprintf(line, sizeof(line), "Timezone: %s%s", TZ_LIST[sel].label, dst ? " (DST)" : "");
  serialmon_append(line);
}

void tz_set_index(int idx) {
  if (idx >= 0 && idx < TZ_COUNT) s_tz_index = idx;
}

int tz_get_index() {
  return s_tz_index;
}

void tz_update_offset_now() {
  uint32_t utc_now = rtc_clock.getCurrentTime();
  DISPLAY_UTC_OFFSET_S = tz_get_effective_offset(s_tz_index, utc_now);
}

// ---- Keyboard focus/defocus ----
void cb_textsend_focused(lv_event_t*) {
  if (!ui_Keyboard1 || !ui_textsendtype) return;
  if (g_in_chat_mode) {
    lv_obj_set_y(ui_textsendtype, KB_TA_Y);
    kb_show(ui_Keyboard1, ui_textsendtype);
    chat_scroll_to_newest();
  }
}

static lv_coord_t s_settings_form_orig_h = 0;

static lv_obj_t* settings_find_scroll_form(lv_obj_t* field) {
  if (!field) return nullptr;
  lv_obj_t* obj = field;
  while (obj) {
    lv_obj_t* parent = lv_obj_get_parent(obj);
    if (!parent) break;
    if (lv_obj_has_flag(parent, LV_OBJ_FLAG_SCROLLABLE)) return parent;
    obj = parent;
  }
  return lv_obj_get_parent(field);
}

static lv_obj_t* settings_field_anchor(lv_obj_t* field, lv_obj_t* form) {
  if (!field || !form) return field;
  lv_obj_t* anchor = field;
  lv_obj_t* parent = lv_obj_get_parent(anchor);
  while (parent && parent != form) {
    anchor = parent;
    parent = lv_obj_get_parent(anchor);
  }
  return anchor;
}

static void settings_scroll_above_keyboard(lv_obj_t* field, lv_obj_t* form) {
  if (!field || !form || !ui_Keyboard2) return;

  lv_obj_t* anchor = settings_field_anchor(field, form);
  if (anchor) lv_obj_scroll_to_view(anchor, LV_ANIM_OFF);

  lv_obj_update_layout(lv_scr_act());

  lv_area_t kb_area;
  lv_area_t anchor_area;
  lv_obj_get_coords(ui_Keyboard2, &kb_area);
  lv_obj_get_coords(anchor ? anchor : field, &anchor_area);

  static const lv_coord_t CLEARANCE = 20;
  lv_coord_t overlap = (anchor_area.y2 + CLEARANCE) - kb_area.y1;
  if (overlap > 0) {
    lv_obj_scroll_by(form, 0, overlap, LV_ANIM_OFF);
  }
}

void settings_field_focus(lv_obj_t* field) {
  if (!field || !ui_Keyboard2) return;
  kb_show(ui_Keyboard2, field);
  lv_obj_t* form = settings_find_scroll_form(field);
  if (!form) return;
  if (s_settings_form_orig_h == 0) s_settings_form_orig_h = lv_obj_get_height(form);
  lv_coord_t form_y = lv_obj_get_y(form);
  lv_coord_t kb_top = SETTINGS_KB_TOP;
  lv_coord_t new_h = kb_top - form_y;
  if (new_h > 100) lv_obj_set_height(form, new_h);
  settings_scroll_above_keyboard(field, form);
}
void settings_field_defocus(lv_obj_t* field) {
  kb_hide(ui_Keyboard2, field);
  if (!field) return;
  lv_obj_t* form = settings_find_scroll_form(field);
  if (form && s_settings_form_orig_h > 0) {
    lv_obj_set_height(form, s_settings_form_orig_h);
    s_settings_form_orig_h = 0;
  }
}

void cb_rename_focused(lv_event_t*)     { settings_field_focus(ui_renamebox); }
void cb_rename_defocused(lv_event_t*)   { settings_field_defocus(ui_renamebox); }
void cb_timeout_focused(lv_event_t*)    { settings_field_focus(ui_screentimeout); }
void cb_txpower_focused(lv_event_t*)    { settings_field_focus(ui_txpowerslider); }
void cb_txpower_defocused(lv_event_t*)  { settings_field_defocus(ui_txpowerslider); }
void cb_hashtag_focused(lv_event_t*)    { settings_field_focus(ui_hashtagchannel); }

void cb_textsend_defocused(lv_event_t*) {
  kb_hide(ui_Keyboard1, ui_textsendtype);
  if (g_in_chat_mode && ui_textsendtype) {
    lv_obj_set_y(ui_textsendtype, TEXTSEND_Y_DEFAULT);
    chat_scroll_to_newest();
  }
}
void cb_timeout_defocused(lv_event_t*)  { settings_field_defocus(ui_screentimeout); }
void cb_hashtag_defocused(lv_event_t*)  { settings_field_defocus(ui_hashtagchannel); }

// ---- Search field ----
void cb_searchfield_focused(lv_event_t*) {
  if (!g_in_chat_mode && ui_Keyboard1 && ui_searchfield)
    kb_show(ui_Keyboard1, ui_searchfield);
}
void cb_searchfield_defocused(lv_event_t*) {
  kb_hide(ui_Keyboard1, ui_searchfield);
}
void cb_searchfield_changed(lv_event_t*) {
  if (!ui_searchfield || g_in_chat_mode) return;
  if (g_del.kind != TargetKind::NONE) return;
  const char* t = lv_textarea_get_text(ui_searchfield);
  strncpy(g_search_filter, t ? t : "", sizeof(g_search_filter) - 1);
  g_search_filter[sizeof(g_search_filter) - 1] = '\0';
  for (char* p = g_search_filter; *p; p++) *p = (char)tolower((unsigned char)*p);
  build_homescreen_list();
}
void cb_searchfield_ready(lv_event_t*) {
  kb_hide(ui_Keyboard1, ui_searchfield);
}

// ---- Ready callbacks ----
void cb_textsend_ready(lv_event_t*) {
  if (!ui_textsendtype) return;
  const char* t = lv_textarea_get_text(ui_textsendtype);
  if (t && *t) {
    strncpy(g_deferred_send_text, t, sizeof(g_deferred_send_text) - 1);
    g_deferred_send_text[sizeof(g_deferred_send_text) - 1] = '\0';
    g_deferred_send_pending = true;
  }
  lv_textarea_set_text(ui_textsendtype, "");
  kb_hide(ui_Keyboard1, ui_textsendtype);
  lv_obj_set_y(ui_textsendtype, TEXTSEND_Y_DEFAULT);
}

void cb_rename_ready(lv_event_t*) {
  const char* t = ui_renamebox ? lv_textarea_get_text(ui_renamebox) : "";
  mesh_rename_if_non_empty(t);
  if (ui_renamebox) lv_textarea_set_text(ui_renamebox, "");
  settings_field_defocus(ui_renamebox);
}

void cb_timeout_ready(lv_event_t*) {
  const char* t = ui_screentimeout ? lv_textarea_get_text(ui_screentimeout) : "";
  uint32_t s = parse_u32(t, 30);
  s = clamp_timeout_s(s);
  save_timeout_s(s);

  if (ui_screentimeout) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)s);
    lv_textarea_set_text(ui_screentimeout, buf);
  }
  serialmon_append("Screen timeout saved");
  settings_field_defocus(ui_screentimeout);
}

void cb_hashtag_ready(lv_event_t*) {
  if (!g_mesh || !ui_hashtagchannel) {
    settings_field_defocus(ui_hashtagchannel);
    return;
  }

  String tag = normalize_hashtag(lv_textarea_get_text(ui_hashtagchannel));
  if (tag.length()) {
    int idx = mesh_join_hashtag_channel(tag);
    ui_refresh_targets();

    if (idx >= 0) {
      enter_chat_mode(TargetKind::CHANNEL, idx, nullptr);
    }
  }

  lv_textarea_set_text(ui_hashtagchannel, "");
  settings_field_defocus(ui_hashtagchannel);
}

// ---- Purge data ----
void purge_btn_restore_position() {
  if (!ui_purgedatabutton) return;
  lv_obj_t* parent = lv_obj_get_parent(ui_purgedatabutton);
  if (!parent) return;
  // Move back to the end (its original position in DANGER ZONE)
  lv_obj_move_to_index(ui_purgedatabutton, lv_obj_get_child_cnt(parent) - 1);
  lv_obj_set_style_bg_color(ui_purgedatabutton, lv_color_hex(g_theme->btn_danger), 0);
  lv_obj_t* lbl = lv_obj_get_child(ui_purgedatabutton, 0);
  if (lbl) lv_label_set_text(lbl, LV_SYMBOL_TRASH "\nPurge Data");
}

void cb_purge_data(lv_event_t*) {
  if (!g_mesh) return;
  if (confirm_is_valid(ConfirmAction::PURGE_DATA)) {
    mesh_purge_contacts_and_repeaters();
    confirm_clear();
    purge_btn_restore_position();
    g_repeater_count = 0;
    repeater_reset_state();
    if (ui_repeatersdropdown)
      lv_dropdown_set_options(ui_repeatersdropdown, "(no repeaters found)");
    serialmon_append("Purge Data: deleted all contacts and repeaters");
    serialmon_append("Please reboot the device.");
    ui_refresh_targets();
    return;
  }
  // Move the button right after serial monitor so the user can see the prompt
  if (ui_purgedatabutton && ui_serialmonitorwindow) {
    int serial_idx = lv_obj_get_index(ui_serialmonitorwindow);
    lv_obj_move_to_index(ui_purgedatabutton, serial_idx + 1);
    // Flash the button to indicate "press again"
    lv_obj_set_style_bg_color(ui_purgedatabutton, lv_color_hex(0xFF0000), 0);
    lv_obj_t* lbl = lv_obj_get_child(ui_purgedatabutton, 0);
    if (lbl) lv_label_set_text(lbl, LV_SYMBOL_TRASH "\nTap again to CONFIRM");
    // Scroll so serial monitor + confirm button are visible
    lv_obj_t* parent = lv_obj_get_parent(ui_purgedatabutton);
    if (parent) lv_obj_scroll_to_view(ui_serialmonitorwindow, LV_ANIM_ON);
  }
  confirm_start(ConfirmAction::PURGE_DATA, nullptr,
                "Press Purge Data again to delete all contacts and repeaters");
}

// ---- Presets ----
void ui_populate_presets() {
  if (!ui_presetsdropdown || !g_mesh) return;
  int n = 0;
  auto p = mesh_presets(n);
  dd_opts_buf[0] = '\0';
  for (int i = 0; i < n; i++) {
    strncat(dd_opts_buf, p[i].name, sizeof(dd_opts_buf) - strlen(dd_opts_buf) - 2);
    if (i != n - 1) strcat(dd_opts_buf, "\n");
  }
  lv_dropdown_set_options(ui_presetsdropdown, dd_opts_buf);
  int saved_idx = load_preset_idx_nvs();
  if (saved_idx >= 0 && saved_idx < n)
    lv_dropdown_set_selected(ui_presetsdropdown, (uint16_t)saved_idx);
  lv_obj_set_style_text_font(ui_presetsdropdown, &lv_font_montserrat_16, 0);
  lv_obj_t* list = lv_dropdown_get_list(ui_presetsdropdown);
  if (list) lv_obj_set_style_text_font(list, &lv_font_montserrat_16, 0);
}

// ============================================================
// WiFi callbacks
// ============================================================

static lv_timer_t* s_wifi_scan_timer = nullptr;

void ui_apply_wifi_state() {
  if (!ui_wifitoggle) return;
  lv_obj_set_style_bg_color(ui_wifitoggle,
    g_wifi_enabled ? lv_color_hex(g_theme->btn_active) : lv_color_hex(g_theme->btn_danger), 0);
  if (!ui_wifitoggle_lbl) return;

  // Full WiFi status is rendered inside the toggle button itself — the
  // separate ui_wifistatuslabel is kept empty/hidden by wifi_ui_update_status.
  // Use LVGL label recolor so the "Connected to: SSID" line can be green.
  lv_label_set_recolor(ui_wifitoggle_lbl, true);

  if (!g_wifi_enabled) {
    lv_label_set_text(ui_wifitoggle_lbl, LV_SYMBOL_WIFI "  WiFi OFF");
  } else if (g_wifi_connected) {
    char buf[224];
    snprintf(buf, sizeof(buf),
             LV_SYMBOL_WIFI "  WiFi ON\n#6DC264 Connected to: %s#\nVisit: %s for GPS and Dashboard",
             g_wifi_ssid,
             WiFi.localIP().toString().c_str());
    lv_label_set_text(ui_wifitoggle_lbl, buf);
  } else if (g_wifi_has_saved_network) {
    char buf[96];
    snprintf(buf, sizeof(buf),
             LV_SYMBOL_WIFI "  WiFi ON\nSaved: %s (not connected)",
             g_wifi_ssid);
    lv_label_set_text(ui_wifitoggle_lbl, buf);
  } else {
    lv_label_set_text(ui_wifitoggle_lbl, LV_SYMBOL_WIFI "  WiFi ON\nNo network configured");
  }
}

static void wifi_show_network_ui(bool show) {
  if (ui_wifinetworksdropdown) { if (show) lv_obj_clear_flag(ui_wifinetworksdropdown, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(ui_wifinetworksdropdown, LV_OBJ_FLAG_HIDDEN); }
  if (ui_wifipassword) { if (show) lv_obj_clear_flag(ui_wifipassword, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(ui_wifipassword, LV_OBJ_FLAG_HIDDEN); }
  if (ui_wificonnectbutton) { if (show) lv_obj_clear_flag(ui_wificonnectbutton, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(ui_wificonnectbutton, LV_OBJ_FLAG_HIDDEN); }
}

void wifi_ui_update_status() {
  // Status text now lives inside the WiFi toggle button itself.
  ui_apply_wifi_state();

  // Keep the separate label empty/hidden (historical artefact).
  if (ui_wifistatuslabel) {
    lv_label_set_text(ui_wifistatuslabel, "");
    lv_obj_add_flag(ui_wifistatuslabel, LV_OBJ_FLAG_HIDDEN);
  }

  // Show/hide forget button
  if (ui_wififorgetbutton) {
    if (g_wifi_has_saved_network)
      lv_obj_clear_flag(ui_wififorgetbutton, LV_OBJ_FLAG_HIDDEN);
    else
      lv_obj_add_flag(ui_wififorgetbutton, LV_OBJ_FLAG_HIDDEN);
  }
}

void cb_wifi_toggle(lv_event_t*) {
  wifi_toggle(!g_wifi_enabled);
  ui_apply_wifi_state();
  wifi_ui_update_status();
  serialmon_append(g_wifi_enabled ? "WiFi: ON" : "WiFi: OFF");
}

void cb_wifi_scan(lv_event_t*) {
  if (!g_wifi_enabled) {
    serialmon_append("Enable WiFi first, then scan");
    return;
  }
  wifi_request_scan();
  if (ui_wifiscan_lbl) lv_label_set_text(ui_wifiscan_lbl, LV_SYMBOL_REFRESH " Scanning...");
  if (ui_wifiscanbutton) lv_obj_add_state(ui_wifiscanbutton, LV_STATE_DISABLED);

  // Poll for results from the deferred synchronous scan
  if (s_wifi_scan_timer) lv_timer_del(s_wifi_scan_timer);
  s_wifi_scan_timer = lv_timer_create([](lv_timer_t* t) {
    if (!wifi_scan_results_ready()) return;

    int n = wifi_scan_result_count();
    if (n > 0) {
      static char scan_opts[1024];
      scan_opts[0] = '\0';
      for (int i = 0; i < n && i < 20; i++) {
        String ssid = wifi_scan_ssid(i);
        int rssi = wifi_scan_rssi(i);
        if (ssid.length() == 0) continue;
        char line[80];
        snprintf(line, sizeof(line), "%s (%d dBm)", ssid.c_str(), rssi);
        if (scan_opts[0]) strncat(scan_opts, "\n", sizeof(scan_opts) - strlen(scan_opts) - 1);
        strncat(scan_opts, line, sizeof(scan_opts) - strlen(scan_opts) - 1);
      }
      if (ui_wifinetworksdropdown) {
        lv_dropdown_set_options(ui_wifinetworksdropdown, scan_opts);
        lv_dropdown_set_selected(ui_wifinetworksdropdown, 0);
      }
      wifi_show_network_ui(true);
    } else {
      serialmon_append("WiFi: no networks found - try again");
      wifi_show_network_ui(false);
    }

    wifi_scan_results_consumed();
    if (ui_wifiscan_lbl) lv_label_set_text(ui_wifiscan_lbl, LV_SYMBOL_REFRESH " Scan");
    if (ui_wifiscanbutton) lv_obj_clear_state(ui_wifiscanbutton, LV_STATE_DISABLED);
    lv_timer_del(t);
    s_wifi_scan_timer = nullptr;
  }, 500, nullptr);
}

void cb_wifi_connect(lv_event_t*) {
  if (!ui_wifinetworksdropdown || !ui_wifipassword) return;

  // Get selected SSID (strip the RSSI part)
  char selected[80];
  lv_dropdown_get_selected_str(ui_wifinetworksdropdown, selected, sizeof(selected));
  // Trim " (xxx dBm)" suffix
  char* paren = strrchr(selected, '(');
  if (paren && paren > selected) {
    *(paren - 1) = '\0';  // remove trailing space + (...)
  }

  const char* pass = lv_textarea_get_text(ui_wifipassword);

  wifi_save_credentials(selected, pass ? pass : "");
  wifi_connect_saved();

  // Hide network selection UI
  wifi_show_network_ui(false);
  lv_textarea_set_text(ui_wifipassword, "");

  char msg[64];
  snprintf(msg, sizeof(msg), "WiFi: connecting to %s...", selected);
  serialmon_append(msg);

  wifi_ui_update_status();
  features_field_defocus(ui_wifipassword);
}

void cb_wifi_password_ready(lv_event_t*) {
  cb_wifi_connect(nullptr);
}

void cb_wifi_password_focused(lv_event_t*) {
  features_field_focus(ui_wifipassword);
}

void cb_wifi_password_defocused(lv_event_t*) {
  features_field_defocus(ui_wifipassword);
}

void cb_wifi_forget(lv_event_t*) {
  wifi_forget_credentials();
  wifi_show_network_ui(false);
  wifi_ui_update_status();
  serialmon_append("WiFi: credentials forgotten");
}
