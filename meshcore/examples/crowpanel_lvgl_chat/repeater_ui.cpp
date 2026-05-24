// ============================================================
// repeater_ui.cpp — Repeater screen, login, status, neighbours
// ============================================================

#include "repeater_ui.h"
#include "app_globals.h"
#include "utils.h"
#include "display.h"
#include "persistence.h"
#include "mesh_api.h"

#include <lvgl.h>
#include <RTClib.h>

// SquareLine UI widget externs
#include "ui.h"
#include "ui_repeaterscreen.h"

// ---- helpers ----

void repeater_update_monitor(const char* txt) {
  if (!txt) return;
  strncpy(g_deferred_repeater_mon, txt, sizeof(g_deferred_repeater_mon) - 1);
  g_deferred_repeater_mon[sizeof(g_deferred_repeater_mon) - 1] = '\0';
  g_deferred_repeater_mon_dirty = true;
}

void clear_channel_receipt_poll() {
  g_channel_receipt.active = false;
  g_channel_receipt.heard_count = 0;
  g_channel_receipt.status_label = nullptr;
  g_channel_receipt.chat_key = "";
  g_channel_receipt.discover_tag = 0;
  g_channel_receipt.timeout_ms = 0;
  g_channel_receipt.seen_count = 0;
}

// Count nearby (0-hop) repeaters from the contact list
int count_zero_hop_repeaters() {
  if (!g_mesh) return 0;
  int count = 0;
  ContactsIterator it;
  ContactInfo c;
  while (it.hasNext(g_mesh, c)) {
    if (c.type == ADV_TYPE_REPEATER && c.out_path_len == 0)
      count++;
  }
  return count;
}

void poll_channel_receipt_if_due() {
  if (!g_channel_receipt.active || g_channel_receipt.timeout_ms == 0) return;
  if (millis() < g_channel_receipt.timeout_ms) return;

  // Timeout reached — finalize the receipt count
  g_channel_receipt.heard_count = (uint16_t)g_channel_receipt.seen_count;
  g_channel_receipt.discover_tag = 0;
  g_channel_receipt.timeout_ms = 0;
  g_deferred_receipt_update = true;

  // Also update the persisted chat file
  if (g_channel_receipt.chat_key.length()) {
    update_last_tx_status_in_file(g_channel_receipt.chat_key, 'R',
                                  (int16_t)g_channel_receipt.heard_count);
  }
}

void repeater_populate_dropdown() {
  if (!ui_repeatersdropdown || !g_mesh) return;

  g_repeater_count = 0;
  ContactsIterator it;
  ContactInfo c;
  while (it.hasNext(g_mesh, c)) {
    if (c.type == ADV_TYPE_REPEATER && g_repeater_count < MAX_DD_ENTRIES) {
      if (g_repeater_filter[0]) {
        char lower[32]; strncpy(lower, c.name, 31); lower[31] = '\0';
        for (char* p = lower; *p; p++) *p = (char)tolower((unsigned char)*p);
        if (!strstr(lower, g_repeater_filter)) continue;
      }
      g_repeater_list[g_repeater_count++] = g_mesh->lookupContactByPubKey(c.id.pub_key, 32);
    }
  }

  static char opts[MAX_DD_ENTRIES * 36];
  opts[0] = '\0';
  for (int i = 0; i < g_repeater_count; i++) {
    if (g_repeater_list[i]) {
      strncat(opts, g_repeater_list[i]->name, sizeof(opts) - strlen(opts) - 2);
    } else {
      strncat(opts, "???", sizeof(opts) - strlen(opts) - 2);
    }
    if (i < g_repeater_count - 1) strcat(opts, "\n");
  }
  if (g_repeater_count == 0) strcpy(opts, "(no repeaters found)");

  if (ui_repeatersdropdown) lv_dropdown_set_options(ui_repeatersdropdown, opts);
}

void repeater_set_action_buttons_visible(bool visible) {
  auto fn = visible ? lv_obj_clear_flag : lv_obj_add_flag;
  if (ui_repeateradvertbutton) fn(ui_repeateradvertbutton, LV_OBJ_FLAG_HIDDEN);
  if (ui_neighboursbutton)     fn(ui_neighboursbutton,     LV_OBJ_FLAG_HIDDEN);
  if (ui_rebootbutton)         fn(ui_rebootbutton,         LV_OBJ_FLAG_HIDDEN);
  if (ui_statusbutton)         fn(ui_statusbutton,         LV_OBJ_FLAG_HIDDEN);
}

static void cb_repeater_refresh(lv_event_t*) {
  if (!g_repeater_logged_in || !g_selected_repeater || !g_mesh) return;
  uint32_t tag, est_timeout;
  g_mesh->sendRequest(*g_selected_repeater, (uint8_t)0x01, tag, est_timeout);
  g_status_pending_key = tag;
  repeater_update_monitor("Refreshing status...");
}

void repeater_reset_state() {
  if (g_selected_repeater && g_repeater_logged_in && g_mesh)
    mesh_stop_repeater_connection(g_selected_repeater->id.pub_key);
  clear_channel_receipt_poll();
  g_selected_repeater  = nullptr;
  g_repeater_logged_in = false;
  memset(g_login_pending_key, 0, 4);
  g_login_timeout_ms       = 0;
  g_login_last_pw[0]       = '\0';
  g_status_pending_key     = 0;
  g_neighbours_pending_key = 0;
}

void repeater_reset_login() {
  repeater_reset_state();
  repeater_set_action_buttons_visible(false);
  repeater_update_monitor("Select a repeater and enter password to login.");
}

// ---- callbacks ----
static void cb_repeater_search_focused(lv_event_t*) {
  if (ui_Keyboard3 && ui_repeatersearchfield) kb_show(ui_Keyboard3, ui_repeatersearchfield);
}
static void cb_repeater_search_defocused(lv_event_t*) {
  kb_hide(ui_Keyboard3, ui_repeatersearchfield);
}
static void cb_repeater_search_changed(lv_event_t*) {
  if (!ui_repeatersearchfield) return;
  const char* t = lv_textarea_get_text(ui_repeatersearchfield);
  strncpy(g_repeater_filter, t ? t : "", sizeof(g_repeater_filter) - 1);
  g_repeater_filter[sizeof(g_repeater_filter) - 1] = '\0';
  for (char* p = g_repeater_filter; *p; p++) *p = (char)tolower((unsigned char)*p);
  repeater_populate_dropdown();
}
static void cb_repeater_search_ready(lv_event_t*) {
  kb_hide(ui_Keyboard3, ui_repeatersearchfield);
}

static void cb_repeater_dropdown_changed(lv_event_t*) {
  if (!ui_repeatersdropdown || g_repeater_count == 0) return;
  uint16_t sel = lv_dropdown_get_selected(ui_repeatersdropdown);
  if (sel < (uint16_t)g_repeater_count && g_repeater_list[sel]) {
    g_selected_repeater  = g_repeater_list[sel];
    g_repeater_logged_in = false;
    memset(g_login_pending_key, 0, 4);
    if (ui_repeaterpassword) lv_textarea_set_text(ui_repeaterpassword, "");
    char msg[96];
    snprintf(msg, sizeof(msg), "Selected: %s\nEnter password to login.", g_selected_repeater->name);
    repeater_update_monitor(msg);
  }
}

static void cb_repeater_pw_focused(lv_event_t*) {
  if (ui_Keyboard3 && ui_repeaterpassword) kb_show(ui_Keyboard3, ui_repeaterpassword);
}
static void cb_repeater_pw_defocused(lv_event_t*) {
  kb_hide(ui_Keyboard3, ui_repeaterpassword);
}

static void cb_repeater_login(lv_event_t*) {
  if (!g_selected_repeater || !g_mesh) return;
  kb_hide(ui_Keyboard3, ui_repeaterpassword);

  if (ui_repeaterloginbutton) {
    lv_obj_add_state(ui_repeaterloginbutton, LV_STATE_DISABLED);
    lv_timer_t* t = lv_timer_create([](lv_timer_t* t) {
      if (ui_repeaterloginbutton) lv_obj_clear_state(ui_repeaterloginbutton, LV_STATE_DISABLED);
      lv_timer_del(t);
    }, 3000, nullptr);
    (void)t;
  }

  const char* pw = "";
  if (ui_repeaterpassword) pw = lv_textarea_get_text(ui_repeaterpassword);
  strncpy(g_login_last_pw, pw, sizeof(g_login_last_pw) - 1);
  g_login_last_pw[sizeof(g_login_last_pw) - 1] = '\0';

  ContactInfo flood_target = *g_selected_repeater;
  flood_target.out_path_len = OUT_PATH_UNKNOWN;

  uint32_t est_timeout;
  int result = g_mesh->sendLogin(flood_target, pw, est_timeout);
  {
    char dbg[48];
    snprintf(dbg, sizeof(dbg), "sendLogin result=%d est_timeout=%lu", result, (unsigned long)est_timeout);
    serialmon_append(dbg);
  }
  if (result == MSG_SEND_FAILED) {
    repeater_update_monitor("Login failed to send - radio busy.");
    return;
  }
  memcpy(g_login_pending_key, g_selected_repeater->id.pub_key, 4);
  g_login_retry_count = 0;
  g_login_timeout_ms = millis() + max(est_timeout * 4, (uint32_t)20000);
  repeater_update_monitor("Login sent, waiting for response...");
}

static void cb_repeater_advert(lv_event_t*) {
  if (!g_repeater_logged_in || !g_selected_repeater || !g_mesh) return;
  uint32_t est_timeout;
  g_mesh->sendCommandData(*g_selected_repeater,
    g_mesh->getRTCClock()->getCurrentTime(), 0, "advert", est_timeout);
  repeater_update_monitor("Advert command sent.");
}

static void cb_repeater_neighbours(lv_event_t*) {
  if (!g_repeater_logged_in || !g_selected_repeater || !g_mesh) return;
  uint8_t req[11];
  req[0] = 0x06;
  req[1] = 0;
  req[2] = 20;
  req[3] = 0; req[4] = 0;
  req[5] = 0;
  req[6] = 4;
  req[7] = 0; req[8] = 0; req[9] = 0; req[10] = 0;
  uint32_t tag, est_timeout;
  int result = g_mesh->sendRequest(*g_selected_repeater, req, sizeof(req), tag, est_timeout);
  if (result != MSG_SEND_FAILED) {
    g_neighbours_pending_key = tag;
    repeater_update_monitor("Neighbours request sent, waiting...");
  } else {
    repeater_update_monitor("Neighbours request failed to send.");
  }
}

static void cb_repeater_reboot(lv_event_t*) {
  if (!g_repeater_logged_in || !g_selected_repeater || !g_mesh) return;
  uint32_t est_timeout;
  g_mesh->sendCommandData(*g_selected_repeater,
    g_mesh->getRTCClock()->getCurrentTime(), 0, "reboot", est_timeout);
  repeater_update_monitor("Reboot command sent.");
}

void setup_repeater_screen_callbacks() {
  if (ui_repeatermonitor) {
    lv_label_set_long_mode(ui_repeatermonitor, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(ui_repeatermonitor, lv_color_hex(0xE0E0E0), 0);
    lv_obj_set_style_text_font(ui_repeatermonitor, &lv_font_montserrat_14, 0);
    lv_label_set_text(ui_repeatermonitor, "Select a repeater and enter password to login.");
  }
  repeater_set_action_buttons_visible(false);

  if (ui_repeatersdropdown) {
    lv_obj_add_event_cb(ui_repeatersdropdown, cb_repeater_dropdown_changed, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_set_style_text_font(ui_repeatersdropdown, &lv_font_montserrat_16, 0);
    lv_obj_t* list = lv_dropdown_get_list(ui_repeatersdropdown);
    if (list) lv_obj_set_style_text_font(list, &lv_font_montserrat_16, 0);
  }

  if (ui_repeatersearchfield) {
    lv_obj_add_event_cb(ui_repeatersearchfield, cb_repeater_search_focused,  LV_EVENT_FOCUSED,       nullptr);
    lv_obj_add_event_cb(ui_repeatersearchfield, cb_repeater_search_focused,  LV_EVENT_CLICKED,       nullptr);
    lv_obj_add_event_cb(ui_repeatersearchfield, cb_repeater_search_defocused,LV_EVENT_DEFOCUSED,     nullptr);
    lv_obj_add_event_cb(ui_repeatersearchfield, cb_repeater_search_changed,  LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(ui_repeatersearchfield, cb_repeater_search_ready,    LV_EVENT_READY,         nullptr);
  }

  if (ui_repeaterpassword) {
    lv_obj_set_style_text_font(ui_repeaterpassword, &lv_font_montserrat_16, 0);
    lv_obj_add_event_cb(ui_repeaterpassword, cb_repeater_pw_focused,   LV_EVENT_FOCUSED,   nullptr);
    lv_obj_add_event_cb(ui_repeaterpassword, cb_repeater_pw_focused,   LV_EVENT_CLICKED,   nullptr);
    lv_obj_add_event_cb(ui_repeaterpassword, cb_repeater_pw_defocused, LV_EVENT_DEFOCUSED, nullptr);
  }

  if (ui_repeaterloginbutton)
    lv_obj_add_event_cb(ui_repeaterloginbutton,  cb_repeater_login,      LV_EVENT_CLICKED, nullptr);
  if (ui_statusbutton)
    lv_obj_add_event_cb(ui_statusbutton,         cb_repeater_refresh,    LV_EVENT_CLICKED, nullptr);
  if (ui_repeateradvertbutton)
    lv_obj_add_event_cb(ui_repeateradvertbutton, cb_repeater_advert,     LV_EVENT_CLICKED, nullptr);
  if (ui_neighboursbutton)
    lv_obj_add_event_cb(ui_neighboursbutton,     cb_repeater_neighbours, LV_EVENT_CLICKED, nullptr);
  if (ui_rebootbutton)
    lv_obj_add_event_cb(ui_rebootbutton,         cb_repeater_reboot,     LV_EVENT_CLICKED, nullptr);

  setup_keyboard(ui_Keyboard3);
  if (ui_Keyboard3) lv_obj_add_flag(ui_Keyboard3, LV_OBJ_FLAG_HIDDEN);
}

static void cb_repeater_screen_deleted(lv_event_t*) {
  g_repeater_cbs_wired = false;
}

void cb_repeater_screen_loaded(lv_event_t*) {
  if (!g_repeater_cbs_wired) {
    setup_repeater_screen_callbacks();
    lv_obj_add_event_cb(ui_repeaterscreen, cb_repeater_screen_deleted,
                        LV_EVENT_DELETE, nullptr);
    g_repeater_cbs_wired = true;
  }
  bool login_pending = (g_login_pending_key[0] | g_login_pending_key[1] |
                        g_login_pending_key[2] | g_login_pending_key[3]) != 0;
  if (!login_pending && !g_repeater_logged_in)
    repeater_reset_login();
  repeater_populate_dropdown();
}
