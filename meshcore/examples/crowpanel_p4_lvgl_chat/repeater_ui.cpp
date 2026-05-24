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

// SquareLine UI widget externs
#include "ui.h"
#include "ui_repeaterscreen.h"

// ---- helpers ----

// Pubkey of the most-recently-selected repeater. Captured in
// cb_repeater_dropdown_changed and used by refresh_selected_repeater()
// to re-resolve g_selected_repeater on every action click. This guards
// against a dangling pointer scenario: BaseChatMesh::removeContact()
// (BaseChatMesh.cpp:779) shuffles contacts[] down on delete, so any
// ContactInfo* held across that call now points at a different
// contact. Re-resolving by pubkey on every click keeps g_selected_repeater
// pointing at the right slot regardless of array shuffles.
static uint8_t s_selected_repeater_pubkey[32] = {0};
static bool    s_selected_repeater_pubkey_set = false;
// Copied name snapshot — readable from any task / endpoint without
// dereferencing g_selected_repeater. See repeater_get_selected_name().
static char    s_selected_repeater_name[32]  = {0};

// Sticky repeater cache so manually-discovered repeaters do not vanish
// from the dropdown as soon as the live contact array churns.
struct StickyRepeaterEntry {
  uint8_t pub_key[32];
  char    name[32];
  uint8_t out_path_len;
  uint32_t last_seen_ms;
};
static StickyRepeaterEntry s_sticky_repeaters[MAX_DD_ENTRIES] = {};
static int                 s_sticky_repeater_count = 0;
static constexpr uint32_t  kStickyRepeaterTtlMs = 24UL * 60UL * 60UL * 1000UL; // 24h

static int sticky_find_idx(const uint8_t pub_key[32]) {
  if (!pub_key) return -1;
  for (int i = 0; i < s_sticky_repeater_count; ++i) {
    if (memcmp(s_sticky_repeaters[i].pub_key, pub_key, 32) == 0) return i;
  }
  return -1;
}

static void sticky_touch(const uint8_t pub_key[32], const char* name, uint8_t out_path_len) {
  if (!pub_key) return;
  const uint32_t now = millis();
  int idx = sticky_find_idx(pub_key);
  if (idx < 0) {
    if (s_sticky_repeater_count < MAX_DD_ENTRIES) {
      idx = s_sticky_repeater_count++;
    } else {
      // Replace the oldest cache entry when full.
      idx = 0;
      uint32_t now_age_oldest = now - s_sticky_repeaters[0].last_seen_ms;
      for (int i = 1; i < s_sticky_repeater_count; ++i) {
        uint32_t age = now - s_sticky_repeaters[i].last_seen_ms;
        if (age > now_age_oldest) {
          now_age_oldest = age;
          idx = i;
        }
      }
    }
    memcpy(s_sticky_repeaters[idx].pub_key, pub_key, 32);
  }
  if (name && name[0]) {
    strncpy(s_sticky_repeaters[idx].name, name, sizeof(s_sticky_repeaters[idx].name) - 1);
    s_sticky_repeaters[idx].name[sizeof(s_sticky_repeaters[idx].name) - 1] = '\0';
  } else if (!s_sticky_repeaters[idx].name[0]) {
    strcpy(s_sticky_repeaters[idx].name, "(unknown)");
  }
  s_sticky_repeaters[idx].out_path_len = out_path_len;
  s_sticky_repeaters[idx].last_seen_ms = now;
}

static void sticky_prune_expired() {
  const uint32_t now = millis();
  int wr = 0;
  for (int rd = 0; rd < s_sticky_repeater_count; ++rd) {
    const uint32_t age = now - s_sticky_repeaters[rd].last_seen_ms;
    const bool keep = (age <= kStickyRepeaterTtlMs) ||
                      (s_selected_repeater_pubkey_set &&
                       memcmp(s_sticky_repeaters[rd].pub_key, s_selected_repeater_pubkey, 32) == 0);
    if (!keep) continue;
    if (wr != rd) s_sticky_repeaters[wr] = s_sticky_repeaters[rd];
    ++wr;
  }
  s_sticky_repeater_count = wr;
}

static bool name_matches_repeater_filter(const char* name) {
  if (!g_repeater_filter[0]) return true;
  if (!name) return false;
  char lower[32];
  strncpy(lower, name, sizeof(lower) - 1);
  lower[sizeof(lower) - 1] = '\0';
  for (char* p = lower; *p; ++p) *p = (char)tolower((unsigned char)*p);
  return strstr(lower, g_repeater_filter) != nullptr;
}

// Returns a freshly-resolved pointer or nullptr if the saved pubkey no
// longer matches any contact (e.g. the repeater was removed). Updates
// g_selected_repeater on success so the rest of the file's existing
// uses of g_selected_repeater see a valid pointer for this call.
static ContactInfo* refresh_selected_repeater() {
  if (!g_mesh || !s_selected_repeater_pubkey_set) return nullptr;
  ContactInfo* fresh = g_mesh->lookupContactByPubKey(s_selected_repeater_pubkey, 32);
  g_selected_repeater = fresh;
  return fresh;
}

bool repeater_get_selected_pubkey(uint8_t out[32]) {
  if (!s_selected_repeater_pubkey_set) return false;
  memcpy(out, s_selected_repeater_pubkey, 32);
  return true;
}

const char* repeater_get_selected_name() {
  return s_selected_repeater_pubkey_set ? s_selected_repeater_name : "";
}

void repeater_set_selected_snapshot(const uint8_t pubkey[32], const char* name) {
  if (!pubkey) return;
  memcpy(s_selected_repeater_pubkey, pubkey, 32);
  s_selected_repeater_pubkey_set = true;
  if (name && name[0]) {
    strncpy(s_selected_repeater_name, name, sizeof(s_selected_repeater_name) - 1);
    s_selected_repeater_name[sizeof(s_selected_repeater_name) - 1] = '\0';
  } else {
    s_selected_repeater_name[0] = '\0';
  }
  g_selected_repeater = g_mesh ? g_mesh->lookupContactByPubKey(s_selected_repeater_pubkey, 32) : nullptr;
}

void repeater_update_monitor(const char* txt) {
  if (!txt) return;
  strncpy(g_deferred_repeater_mon, txt, sizeof(g_deferred_repeater_mon) - 1);
  g_deferred_repeater_mon[sizeof(g_deferred_repeater_mon) - 1] = '\0';
  g_deferred_repeater_mon_dirty = true;
}

// ---- CLI terminal mode ----------------------------------------------------
// When the user taps the CLI button, the password field is hidden and a
// dedicated CLI textarea takes its slot. The on-screen keyboard is
// attached, and any text typed + ENTER is sent to the logged-in repeater
// as a TXT_TYPE_CLI_DATA command. The repeater's reply (delivered by
// onCommandDataRecv in main.cpp) is appended to the terminal buffer
// shown in the monitor area. Pressing CLI again exits.

static bool   s_cli_mode = false;
// Accumulating terminal text. Bounded — when it grows past kCliBufMax,
// drop the oldest ~25% so we never run away with String allocations.
static String s_cli_buf;
static const  size_t kCliBufMax = 3500;
static String s_cli_header_default = "Type a command, press Enter to send.\nResponses appear here.\n";

bool repeater_cli_mode_active() { return s_cli_mode; }

static void set_btn_text(lv_obj_t* btn, const char* txt) {
  if (!btn || !txt) return;
  uint32_t cnt = lv_obj_get_child_cnt(btn);
  for (uint32_t i = 0; i < cnt; i++) {
    lv_obj_t* ch = lv_obj_get_child(btn, i);
    if (ch && lv_obj_check_type(ch, &lv_label_class)) {
      lv_label_set_text(ch, txt);
      return;
    }
  }
}

static void cli_buf_trim_if_needed() {
  if (s_cli_buf.length() <= kCliBufMax) return;
  // Drop the oldest 25% of the buffer; align to the nearest newline so
  // we don't slice through a line and confuse the user.
  size_t drop = s_cli_buf.length() / 4;
  int nl = s_cli_buf.indexOf('\n', drop);
  if (nl > 0 && (size_t)nl + 1 < s_cli_buf.length()) drop = nl + 1;
  s_cli_buf.remove(0, drop);
}

// Append a line to the terminal buffer and push it to the monitor.
// `prefix` is shown before the text (e.g. "$ " for input, "" for
// responses). Newline appended automatically.
static void cli_append_line(const char* prefix, const char* text) {
  if (prefix) s_cli_buf += prefix;
  if (text)   s_cli_buf += text;
  s_cli_buf += '\n';
  cli_buf_trim_if_needed();
  repeater_update_monitor(s_cli_buf.c_str());
}

// Public entry point used by main.cpp::onCommandDataRecv to forward
// repeater replies into the terminal. Only appends if (a) we're in CLI
// mode and (b) the response is from the currently selected repeater.
void repeater_cli_handle_response(const uint8_t* sender_pubkey, const char* text) {
  if (!s_cli_mode || !text || !sender_pubkey) return;
  if (!s_selected_repeater_pubkey_set) return;
  if (memcmp(sender_pubkey, s_selected_repeater_pubkey, 32) != 0) return;
  cli_append_line("", text);
}

static void cli_mode_enter() {
  if (s_cli_mode) return;
  s_cli_mode = true;
  // Initialise the terminal buffer with a header so the user knows
  // they're in CLI mode.
  s_cli_buf  = "[CLI mode - ";
  s_cli_buf += s_selected_repeater_name[0] ? s_selected_repeater_name : "repeater";
  s_cli_buf += "]\n";
  s_cli_buf += s_cli_header_default;
  repeater_update_monitor(s_cli_buf.c_str());

  // Swap the password textarea for the CLI textarea.
  if (ui_repeaterpassword)  lv_obj_add_flag(ui_repeaterpassword,    LV_OBJ_FLAG_HIDDEN);
  if (ui_repeaterloginbutton) lv_obj_add_flag(ui_repeaterloginbutton, LV_OBJ_FLAG_HIDDEN);
  if (ui_repeatercliinput) {
    lv_obj_clear_flag(ui_repeatercliinput, LV_OBJ_FLAG_HIDDEN);
    lv_textarea_set_text(ui_repeatercliinput, "");
    if (ui_Keyboard3) {
      lv_keyboard_set_textarea(ui_Keyboard3, ui_repeatercliinput);
      lv_obj_clear_flag(ui_Keyboard3, LV_OBJ_FLAG_HIDDEN);
    }
  }
  set_btn_text(ui_clibutton, "Exit CLI");
}

static void cli_mode_exit() {
  if (!s_cli_mode) return;
  s_cli_mode = false;
  if (ui_repeatercliinput)    lv_obj_add_flag  (ui_repeatercliinput,    LV_OBJ_FLAG_HIDDEN);
  if (ui_Keyboard3)           lv_obj_add_flag  (ui_Keyboard3,           LV_OBJ_FLAG_HIDDEN);
  if (ui_Keyboard3)           lv_keyboard_set_textarea(ui_Keyboard3, nullptr);
  if (ui_repeaterpassword)    lv_obj_clear_flag(ui_repeaterpassword,    LV_OBJ_FLAG_HIDDEN);
  if (ui_repeaterloginbutton) lv_obj_clear_flag(ui_repeaterloginbutton, LV_OBJ_FLAG_HIDDEN);
  set_btn_text(ui_clibutton, "CLI");
  // Keep the terminal text on the monitor so the user can read what
  // came back. The next Status/Advert/etc click will overwrite it.
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
  sticky_prune_expired();

  g_repeater_count = 0;
  bool live_flags[MAX_DD_ENTRIES] = {};

  auto add_entry = [&](const uint8_t pub_key[32], const char* name, uint8_t out_path_len, bool is_live) {
    if (!pub_key || !name || !name[0]) return;
    if (!name_matches_repeater_filter(name)) return;
    for (int i = 0; i < g_repeater_count; ++i) {
      if (memcmp(g_repeater_list[i].pub_key, pub_key, 32) == 0) {
        if (is_live) {
          g_repeater_list[i].out_path_len = out_path_len;
          live_flags[i] = true;
        }
        return;
      }
    }
    if (g_repeater_count >= MAX_DD_ENTRIES) return;
    RepeaterListEntry& e = g_repeater_list[g_repeater_count];
    memcpy(e.pub_key, pub_key, 32);
    strncpy(e.name, name, sizeof(e.name) - 1);
    e.name[sizeof(e.name) - 1] = '\0';
    e.out_path_len = out_path_len;
    live_flags[g_repeater_count] = is_live;
    ++g_repeater_count;
  };

  ContactsIterator it;
  ContactInfo c;
  while (it.hasNext(g_mesh, c)) {
    if (c.type != ADV_TYPE_REPEATER) continue;
    sticky_touch(c.id.pub_key, c.name, c.out_path_len);
    add_entry(c.id.pub_key, c.name, c.out_path_len, true);
  }

  // Merge sticky discovered repeaters (cached) so they don't disappear
  // immediately when not currently present in live contacts.
  for (int i = 0; i < s_sticky_repeater_count; ++i) {
    add_entry(s_sticky_repeaters[i].pub_key,
              s_sticky_repeaters[i].name,
              s_sticky_repeaters[i].out_path_len,
              false);
  }

  // Ensure current selection stays visible even if filter/live state changed.
  if (s_selected_repeater_pubkey_set) {
    bool found = false;
    for (int i = 0; i < g_repeater_count; ++i) {
      if (memcmp(g_repeater_list[i].pub_key, s_selected_repeater_pubkey, 32) == 0) {
        found = true;
        break;
      }
    }
    if (!found && g_repeater_count < MAX_DD_ENTRIES &&
        name_matches_repeater_filter(s_selected_repeater_name[0] ? s_selected_repeater_name : "(selected)")) {
      RepeaterListEntry& e = g_repeater_list[g_repeater_count];
      memcpy(e.pub_key, s_selected_repeater_pubkey, 32);
      strncpy(e.name,
              s_selected_repeater_name[0] ? s_selected_repeater_name : "(selected)",
              sizeof(e.name) - 1);
      e.name[sizeof(e.name) - 1] = '\0';
      e.out_path_len = OUT_PATH_UNKNOWN;
      live_flags[g_repeater_count] = false;
      ++g_repeater_count;
    }
  }

  static char opts[MAX_DD_ENTRIES * 36];
  opts[0] = '\0';
  for (int i = 0; i < g_repeater_count; i++) {
    char row[40];
    if (live_flags[i]) {
      snprintf(row, sizeof(row), "%s", g_repeater_list[i].name);
    } else {
      snprintf(row, sizeof(row), "%s *", g_repeater_list[i].name);
    }
    strncat(opts, row, sizeof(opts) - strlen(opts) - 2);
    if (i < g_repeater_count - 1) strcat(opts, "\n");
  }
  if (g_repeater_count == 0) strcpy(opts, "(no repeaters found)");

  if (ui_repeatersdropdown) {
    lv_dropdown_set_options(ui_repeatersdropdown, opts);
    // Preserve visual selection across screen switches/rebuilds.
    if (s_selected_repeater_pubkey_set && g_repeater_count > 0) {
      for (int i = 0; i < g_repeater_count; i++) {
        if (memcmp(g_repeater_list[i].pub_key, s_selected_repeater_pubkey, 32) == 0) {
          lv_dropdown_set_selected(ui_repeatersdropdown, (uint16_t)i);
          break;
        }
      }
    }
  }
}

void repeater_set_action_buttons_visible(bool visible) {
  auto fn = visible ? lv_obj_clear_flag : lv_obj_add_flag;
  if (ui_repeateradvertbutton) fn(ui_repeateradvertbutton, LV_OBJ_FLAG_HIDDEN);
  if (ui_neighboursbutton)     fn(ui_neighboursbutton,     LV_OBJ_FLAG_HIDDEN);
  if (ui_clibutton)            fn(ui_clibutton,            LV_OBJ_FLAG_HIDDEN);
  if (ui_rebootbutton)         fn(ui_rebootbutton,         LV_OBJ_FLAG_HIDDEN);
  if (ui_statusbutton)         fn(ui_statusbutton,         LV_OBJ_FLAG_HIDDEN);
}

static void repeater_set_path_button_visible(bool visible) {
  if (!ui_repeaterpathbutton) return;
  if (visible) lv_obj_clear_flag(ui_repeaterpathbutton, LV_OBJ_FLAG_HIDDEN);
  else         lv_obj_add_flag(ui_repeaterpathbutton,   LV_OBJ_FLAG_HIDDEN);
}

static void cb_repeater_refresh(lv_event_t*) {
  if (!g_repeater_logged_in || !g_mesh) return;
  if (!refresh_selected_repeater()) {
    repeater_update_monitor("Repeater no longer in contacts.");
    return;
  }
  uint32_t tag, est_timeout;
  g_mesh->sendRequest(*g_selected_repeater, (uint8_t)0x01, tag, est_timeout);
  g_status_pending_key = tag;
  repeater_update_monitor("Refreshing status...");
}

void repeater_reset_state() {
  // Use the saved pubkey for the stop-connection call, not the
  // g_selected_repeater pointer — that pointer can be dangling here if
  // contacts were shuffled since selection.
  if (s_selected_repeater_pubkey_set && g_repeater_logged_in && g_mesh) {
    mesh_stop_repeater_connection(s_selected_repeater_pubkey);
  }
  // Make sure CLI mode is closed before clearing selection — the
  // password field needs to come back so the user can log in to
  // another repeater.
  cli_mode_exit();
  s_cli_buf = String();
  clear_channel_receipt_poll();
  // Preserve selected repeater across logout/screen switches so re-login
  // doesn't require reselection. The pointer itself is refreshed lazily by
  // refresh_selected_repeater() on each action.
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
  repeater_set_path_button_visible(false);
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
  if (!ui_repeatersdropdown || g_repeater_count == 0 || !g_mesh) return;
  uint16_t sel = lv_dropdown_get_selected(ui_repeatersdropdown);
  if (sel >= (uint16_t)g_repeater_count) return;
  // The dropdown row stores pubkey + name only. Resolve to a live
  // ContactInfo* now and stash the pubkey + name for re-resolution
  // later (the name is exposed via repeater_get_selected_name() so
  // off-task readers like web_dashboard's monitor endpoint never need
  // to deref the short-lived g_selected_repeater pointer).
  memcpy(s_selected_repeater_pubkey, g_repeater_list[sel].pub_key, 32);
  strncpy(s_selected_repeater_name, g_repeater_list[sel].name,
          sizeof(s_selected_repeater_name) - 1);
  s_selected_repeater_name[sizeof(s_selected_repeater_name) - 1] = '\0';
  s_selected_repeater_pubkey_set = true;
  g_selected_repeater  = g_mesh->lookupContactByPubKey(s_selected_repeater_pubkey, 32);
  // Hard-cancel any in-flight login retries when switching selection.
  g_login_timeout_ms = 0;
  g_login_retry_count = 0;
  g_login_last_pw[0] = '\0';
  g_repeater_logged_in = false;
  memset(g_login_pending_key, 0, 4);
  repeater_set_action_buttons_visible(false);
  repeater_set_path_button_visible(g_selected_repeater != nullptr);
  if (ui_repeaterpassword) lv_textarea_set_text(ui_repeaterpassword, "");
  char msg[96];
  if (g_selected_repeater) {
    snprintf(msg, sizeof(msg), "Selected: %s\nEnter password to login.",
             g_repeater_list[sel].name);
  } else {
    snprintf(msg, sizeof(msg), "Selected: %s\nNot currently heard.\nRun discovery first.",
             g_repeater_list[sel].name);
  }
  repeater_update_monitor(msg);
}

static void cb_repeater_pw_focused(lv_event_t*) {
  if (ui_Keyboard3 && ui_repeaterpassword) kb_show(ui_Keyboard3, ui_repeaterpassword);
}
static void cb_repeater_pw_defocused(lv_event_t*) {
  kb_hide(ui_Keyboard3, ui_repeaterpassword);
}

static void cb_repeater_login(lv_event_t*) {
  if (!g_mesh) return;
  if (!refresh_selected_repeater()) {
    repeater_update_monitor("Repeater no longer in contacts.");
    return;
  }
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

  // Only do pre-login discovery when we genuinely don't have a path.
  // If a usable route already exists, forcing discovery each login adds
  // delay and makes it look like the path wasn't remembered.
  if (g_selected_repeater->out_path_len == OUT_PATH_UNKNOWN) {
    uint8_t req_data[9];
    req_data[0] = 0x03;        // REQ_TYPE_GET_TELEMETRY_DATA
    req_data[1] = ~(0x01);     // inverse permissions: request base telemetry only
    req_data[2] = 0;
    req_data[3] = 0;
    req_data[4] = 0;
    req_data[5] = (uint8_t)random(0x100);
    req_data[6] = (uint8_t)random(0x100);
    req_data[7] = (uint8_t)random(0x100);
    req_data[8] = (uint8_t)random(0x100);

    ContactInfo discover_target = *g_selected_repeater;
    discover_target.out_path_len = OUT_PATH_UNKNOWN;
    uint32_t dtag = 0, dtimeout = 0;
    int dres = g_mesh->sendRequest(discover_target, req_data, sizeof(req_data), dtag, dtimeout);
    char dmsg[96];
    snprintf(dmsg, sizeof(dmsg), "Path discover pre-login res=%d tag=%08lX t=%lums",
             dres, (unsigned long)dtag, (unsigned long)dtimeout);
    serialmon_append(dmsg);
  }

  uint32_t est_timeout = 0;
  int result = g_mesh->sendLogin(*g_selected_repeater, pw, est_timeout);
  // For routed repeaters, also fire a flood copy as a reachability assist.
  // This mirrors companion-like behavior on long/marginal paths where
  // a stale direct route can miss while flood still reaches via neighbors.
  if (result != MSG_SEND_FAILED &&
      g_selected_repeater->out_path_len != OUT_PATH_UNKNOWN &&
      g_selected_repeater->out_path_len > 0) {
    ContactInfo flood_target = *g_selected_repeater;
    flood_target.out_path_len = OUT_PATH_UNKNOWN;
    uint32_t est_timeout_flood = 0;
    int flood_result = g_mesh->sendLogin(flood_target, pw, est_timeout_flood);
    if (flood_result != MSG_SEND_FAILED && est_timeout_flood > est_timeout)
      est_timeout = est_timeout_flood;
    char dbg2[96];
    snprintf(dbg2, sizeof(dbg2), "sendLogin assist flood result=%d est_timeout=%lu",
             flood_result, (unsigned long)est_timeout_flood);
    serialmon_append(dbg2);
  }
  {
    char dbg[80];
    const char* via = (result == MSG_SEND_SENT_FLOOD) ? "flood" :
                      (result == MSG_SEND_SENT_DIRECT) ? "stored path" : "failed";
    snprintf(dbg, sizeof(dbg), "sendLogin %s result=%d est_timeout=%lu",
             via, result, (unsigned long)est_timeout);
    serialmon_append(dbg);
  }
  if (result == MSG_SEND_FAILED) {
    // sendLogin returns FAILED when packet allocation fails (radio queue
    // full or createAnonDatagram couldn't grab a buffer). Treat as
    // "queued for retry" instead of hard failure — set up the pending
    // state so the main-loop retry path re-sends when the queue drains.
    // Users were reporting "Login failed to send" followed by the login
    // actually succeeding a moment later, which is exactly this case.
    memcpy(g_login_pending_key, g_selected_repeater->id.pub_key, 4);
    g_login_retry_count = 0;
    g_login_timeout_ms = millis() + 15000;  // generous, will be refreshed on retry
    repeater_update_monitor("Radio busy — login queued, retrying...");
    return;
  }
  memcpy(g_login_pending_key, g_selected_repeater->id.pub_key, 4);
  g_login_retry_count = 0;
  g_login_timeout_ms = millis() + max(est_timeout * 4, (uint32_t)20000);
  repeater_update_monitor(result == MSG_SEND_SENT_DIRECT
                            ? "Login sent via stored path, waiting..."
                            : "Login sent by flood, waiting...");
}

static void cb_repeater_advert(lv_event_t*) {
  if (!g_repeater_logged_in || !g_mesh) return;
  if (!refresh_selected_repeater()) {
    repeater_update_monitor("Repeater no longer in contacts.");
    return;
  }
  uint32_t est_timeout;
  g_mesh->sendCommandData(*g_selected_repeater,
    g_mesh->getRTCClock()->getCurrentTime(), 0, "advert", est_timeout);
  repeater_update_monitor("Advert command sent.");
}

static void cb_repeater_neighbours(lv_event_t*) {
  if (!g_repeater_logged_in || !g_mesh) return;
  if (!refresh_selected_repeater()) {
    repeater_update_monitor("Repeater no longer in contacts.");
    return;
  }
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

static void cb_repeater_reset_path(lv_event_t*) {
  if (!g_mesh) return;
  if (!refresh_selected_repeater()) {
    repeater_update_monitor("Repeater no longer in contacts.");
    return;
  }
  if (!mesh_reset_repeater_path_by_pubkey(s_selected_repeater_pubkey)) {
    repeater_update_monitor("Path reset failed.");
    return;
  }
  char msg[96];
  snprintf(msg, sizeof(msg), "Path reset to flood for %s.",
           s_selected_repeater_name[0] ? s_selected_repeater_name : "repeater");
  repeater_update_monitor(msg);
  repeater_populate_dropdown();
}

static void cb_repeater_reboot(lv_event_t*) {
  if (!g_repeater_logged_in || !g_mesh) return;
  if (!refresh_selected_repeater()) {
    repeater_update_monitor("Repeater no longer in contacts.");
    return;
  }
  uint32_t est_timeout;
  g_mesh->sendCommandData(*g_selected_repeater,
    g_mesh->getRTCClock()->getCurrentTime(), 0, "reboot", est_timeout);
  repeater_update_monitor("Reboot command sent.");
}

// CLI button — toggles the terminal mode.
static void cb_repeater_cli(lv_event_t*) {
  if (!g_repeater_logged_in || !g_mesh) return;
  if (!refresh_selected_repeater()) {
    repeater_update_monitor("Repeater no longer in contacts.");
    return;
  }
  if (s_cli_mode) cli_mode_exit();
  else            cli_mode_enter();
}

// CLI input — fires when the user presses Enter on the keyboard. Sends
// the typed text to the repeater as a CLI command, echoes it into the
// terminal as `$ <cmd>`, and clears the input ready for the next.
static void cb_cli_input_ready(lv_event_t*) {
  if (!s_cli_mode || !g_mesh || !ui_repeatercliinput) return;
  if (!refresh_selected_repeater()) {
    cli_append_line("", "[error] repeater no longer in contacts");
    return;
  }
  const char* cmd = lv_textarea_get_text(ui_repeatercliinput);
  if (!cmd || !cmd[0]) return;

  cli_append_line("$ ", cmd);

  uint32_t est_timeout;
  int result = g_mesh->sendCommandData(*g_selected_repeater,
                                        g_mesh->getRTCClock()->getCurrentTime(),
                                        0, cmd, est_timeout);
  if (result == MSG_SEND_FAILED) {
    cli_append_line("", "[error] send failed (radio busy)");
  }
  lv_textarea_set_text(ui_repeatercliinput, "");
}

static void cb_cli_input_focused(lv_event_t*) {
  if (ui_Keyboard3 && ui_repeatercliinput) {
    lv_keyboard_set_textarea(ui_Keyboard3, ui_repeatercliinput);
    lv_obj_clear_flag(ui_Keyboard3, LV_OBJ_FLAG_HIDDEN);
  }
}

static void cb_cli_input_defocused(lv_event_t*) {
  if (ui_Keyboard3) {
    lv_keyboard_set_textarea(ui_Keyboard3, nullptr);
    lv_obj_add_flag(ui_Keyboard3, LV_OBJ_FLAG_HIDDEN);
  }
}

void repeater_cli_hide_keyboard() {
  if (!ui_Keyboard3) return;
  lv_keyboard_set_textarea(ui_Keyboard3, nullptr);
  lv_obj_add_flag(ui_Keyboard3, LV_OBJ_FLAG_HIDDEN);
}

// Mirrors the chat-mode tap-outside detection in display.cpp's touch read
// callback. In CLI mode the keyboard is shown without focusing the input, so
// DEFOCUSED never fires on a background tap; instead we geometry-test the press
// against the keyboard + input rects and request a deferred dismiss.
void repeater_cli_dismiss_kb_if_outside(int x, int y) {
  if (!s_cli_mode || !ui_Keyboard3) return;
  if (lv_obj_has_flag(ui_Keyboard3, LV_OBJ_FLAG_HIDDEN)) return;

  bool on_kb = false, on_ta = false;
  {
    lv_area_t a; lv_obj_get_coords(ui_Keyboard3, &a);
    on_kb = (x >= a.x1 && x <= a.x2 && y >= a.y1 && y <= a.y2);
  }
  if (!on_kb && ui_repeatercliinput) {
    lv_area_t a; lv_obj_get_coords(ui_repeatercliinput, &a);
    on_ta = (x >= a.x1 && x <= a.x2 && y >= a.y1 && y <= a.y2);
  }
  if (!on_kb && !on_ta) g_dismiss_keyboard = true;
}

static void cb_repeater_monitor_long_press(lv_event_t*) {
  if (s_cli_mode) cli_mode_exit();
}

void setup_repeater_screen_callbacks() {
  if (ui_repeatermonitor) {
    lv_label_set_long_mode(ui_repeatermonitor, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(ui_repeatermonitor, lv_color_hex(0xE0E0E0), 0);
    lv_obj_set_style_text_font(ui_repeatermonitor, &lv_font_montserrat_14, 0);
    lv_label_set_text(ui_repeatermonitor, "Select a repeater and enter password to login.");
    lv_obj_add_flag(ui_repeatermonitor, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ui_repeatermonitor, cb_repeater_monitor_long_press,
                        LV_EVENT_LONG_PRESSED, nullptr);
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
  if (ui_repeaterpathbutton)
    lv_obj_add_event_cb(ui_repeaterpathbutton,   cb_repeater_reset_path, LV_EVENT_CLICKED, nullptr);
  if (ui_clibutton)
    lv_obj_add_event_cb(ui_clibutton,            cb_repeater_cli,        LV_EVENT_CLICKED, nullptr);
  if (ui_rebootbutton)
    lv_obj_add_event_cb(ui_rebootbutton,         cb_repeater_reboot,     LV_EVENT_CLICKED, nullptr);

  if (ui_repeatercliinput) {
    lv_obj_add_event_cb(ui_repeatercliinput, cb_cli_input_focused, LV_EVENT_FOCUSED, nullptr);
    lv_obj_add_event_cb(ui_repeatercliinput, cb_cli_input_focused, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(ui_repeatercliinput, cb_cli_input_defocused, LV_EVENT_DEFOCUSED, nullptr);
    lv_obj_add_event_cb(ui_repeatercliinput, cb_cli_input_ready,   LV_EVENT_READY,   nullptr);
  }

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
  if (!login_pending && !g_repeater_logged_in && !s_selected_repeater_pubkey_set)
    repeater_reset_login();
  repeater_populate_dropdown();
  if (s_selected_repeater_pubkey_set) {
    // Keep context sticky when user leaves/re-enters repeater screen.
    g_selected_repeater = g_mesh ? g_mesh->lookupContactByPubKey(s_selected_repeater_pubkey, 32) : nullptr;
    repeater_set_path_button_visible(g_selected_repeater != nullptr);
    if (!g_repeater_logged_in && ui_repeaterpassword) {
      char msg[96];
      snprintf(msg, sizeof(msg), "Selected: %s\nEnter password to login.",
               s_selected_repeater_name[0] ? s_selected_repeater_name : "repeater");
      repeater_update_monitor(msg);
    }
  }
}
