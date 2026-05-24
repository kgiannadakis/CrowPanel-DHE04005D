// ============================================================
// home_ui.cpp — Homescreen bubble list, enter/exit chat, delete
// ============================================================

#include "home_ui.h"
#include "chat_ui.h"
#include "utils.h"
#include "persistence.h"
#include "display.h"
#include "app_globals.h"
#include "mesh_api.h"

#include "ui.h"
#include "ui_homescreen.h"
#include "ui_settingscreen.h"

// ---- Confirm/delete state ----
void confirm_start(ConfirmAction a, const char* msg_chat, const char* msg_serial) {
  g_confirm_action = a;
  g_confirm_deadline_ms = millis() + 5000UL;
  if (msg_chat && ui_chatpanel) chat_add(false, msg_chat);
  if (msg_serial) serialmon_append(msg_serial);
}

bool confirm_is_valid(ConfirmAction a) {
  return (g_confirm_action == a) && ((int32_t)(millis() - g_confirm_deadline_ms) <= 0);
}

void confirm_clear() {
  g_confirm_action = ConfirmAction::NONE;
  g_confirm_deadline_ms = 0;
}

// ---- Mute channel ----
void mute_set_label(const char* txt) {
  lv_obj_t* lbl = ui_mutelabel ? ui_mutelabel : lv_obj_get_child(ui_mutebutton, 0);
  if (!lbl) return;
  lv_label_set_text(lbl, txt);
  lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
  lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_center(lbl);
}

void ui_apply_mute_button_state() {
  if (!ui_mutebutton) return;
  int idx = g_mesh ? mesh_current_channel_idx() : -1;
  bool muted = (idx >= 0 && idx < 32 && (g_muted_channel_mask & (1u << idx)));
  lv_obj_set_style_bg_color(ui_mutebutton,
    muted ? lv_color_hex(g_theme->btn_danger) : lv_color_hex(g_theme->btn_active), 0);
  mute_set_label(muted ? LV_SYMBOL_MUTE : LV_SYMBOL_VOLUME_MAX);
}

void cb_mute_toggle(lv_event_t*) {
  if (!g_mesh) return;
  int idx = mesh_current_channel_idx();
  if (idx < 0 || idx >= 32) return;
  uint32_t bit = 1u << idx;
  g_muted_channel_mask ^= bit;
  bool now_muted = (g_muted_channel_mask & bit);
  ui_apply_mute_button_state();
  save_ui_prefs_nvs();
  ChannelDetails ch;
  const char* name = (g_mesh->getChannel(idx, ch) && ch.name[0]) ? ch.name : "channel";
  char line[80];
  snprintf(line, sizeof(line), "%s: %s", name, now_muted ? "MUTED" : "UNMUTED");
  serialmon_append(line);
  ui_refresh_targets();
}

// ---- Long-press delete ----
static const uint32_t DELETE_CONFIRM_MS = 2000;

static void del_warn_show() {
  if (!ui_searchfield) return;
  lv_textarea_set_text(ui_searchfield, "To delete, press again within 2s");
  lv_obj_set_style_text_color(ui_searchfield, lv_color_hex(g_theme->search_warn), 0);
}
static void del_warn_clear() {
  if (!ui_searchfield) return;
  lv_obj_set_style_text_color(ui_searchfield, lv_color_hex(g_theme->search_text), 0);
  lv_textarea_set_text(ui_searchfield, g_search_filter);
  g_del = {};
  g_long_press_just_fired = false;
}
static bool del_matches(const BubbleTapData* d) {
  if (!d || g_del.kind == TargetKind::NONE) return false;
  if (g_del.kind != d->kind) return false;
  if (d->kind == TargetKind::CHANNEL) return g_del.ch_idx == d->channel_idx;
  return memcmp(g_del.pub_key, d->pub_key, 32) == 0;
}

void cb_bubble_long_pressed(lv_event_t* e) {
  BubbleTapData* d = (BubbleTapData*)lv_event_get_user_data(e);
  if (!d || g_in_chat_mode) return;
  if (d->kind == TargetKind::CHANNEL && d->channel_idx == 0) return;
  g_del.kind     = d->kind;
  g_del.ch_idx   = d->channel_idx;
  memcpy(g_del.pub_key, d->pub_key, 32);
  g_del.armed_ms = millis();
  g_long_press_just_fired = true;
  del_warn_show();
}

void cb_bubble_tapped(lv_event_t* e) {
  BubbleTapData* d = (BubbleTapData*)lv_event_get_user_data(e);
  if (!d || !g_mesh) return;

  if (g_long_press_just_fired) {
    g_long_press_just_fired = false;
    return;
  }

  if (del_matches(d) && (millis() - g_del.armed_ms) <= DELETE_CONFIRM_MS) {
    if (d->kind == TargetKind::CONTACT) {
      mesh_delete_contact_by_pubkey(d->pub_key);
    } else if (d->kind == TargetKind::CHANNEL) {
      mesh_delete_channel_by_idx(d->channel_idx);
    }
    del_warn_clear();
    if (g_chat_header_label) {
      lv_obj_del(g_chat_header_label);
      g_chat_header_label = nullptr;
    }
    g_chat_target_name[0] = '\0';
    g_in_chat_mode = false;
    g_current_chat_key[0] = '\0';
    ui_refresh_targets();
    build_homescreen_list();
    return;
  }

  del_warn_clear();
  enter_chat_mode(d->kind, d->channel_idx, d->pub_key);
}

void cb_back_button(lv_event_t*) { if (g_in_chat_mode) exit_chat_mode(); }

// ---- Delete timer check (called from loop) ----
// Exposed via extern in app_globals.h delete_confirm_ms — used directly in main loop

// ---- Create channel bubble ----
static void create_channel_bubble(const DropdownEntry& e, uint16_t unread) {
  if (!ui_chatpanel || g_bubble_pool_count >= MAX_DD_ENTRIES * 2) return;
  BubbleTapData* td = &g_bubble_pool[g_bubble_pool_count++];
  td->kind = TargetKind::CHANNEL; td->channel_idx = e.channel_idx; memset(td->pub_key, 0, 32);

  bool is_hashtag = (e.name[0] == '#');
  bool ch_muted = (e.channel_idx >= 0 && e.channel_idx < 32 &&
                   (g_muted_channel_mask & (1u << e.channel_idx)));

  lv_obj_t* card = lv_obj_create(ui_chatpanel);
  lv_obj_set_width(card, lv_pct(100));
  lv_obj_set_height(card, 64);
  lv_obj_set_style_radius(card, 0, 0);
  lv_obj_set_style_bg_color(card, lv_color_hex(g_theme->hs_channel_bg), 0);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_border_color(card, lv_color_hex(g_theme->hs_channel_border), 0);
  lv_obj_set_style_border_opa(card, LV_OPA_COVER, 0);
  lv_obj_set_style_shadow_opa(card, LV_OPA_TRANSP, 0);
  lv_obj_set_style_pad_left(card, 10, 0);
  lv_obj_set_style_pad_right(card, 10, 0);
  lv_obj_set_style_pad_ver(card, 0, 0);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(card, cb_bubble_tapped,       LV_EVENT_CLICKED,       td);
  lv_obj_add_event_cb(card, cb_bubble_long_pressed, LV_EVENT_LONG_PRESSED,  td);

  lv_obj_t* iconCircle = lv_obj_create(card);
  lv_obj_set_size(iconCircle, 42, 42);
  lv_obj_set_style_radius(iconCircle, 0, 0);
  lv_obj_set_style_bg_color(iconCircle,
    lv_color_hex(is_hashtag ? g_theme->hs_hashtag_icon : g_theme->hs_channel_icon), 0);
  lv_obj_set_style_bg_opa(iconCircle, LV_OPA_COVER, 0);
  lv_obj_set_style_border_opa(iconCircle, LV_OPA_TRANSP, 0);
  lv_obj_set_style_pad_all(iconCircle, 0, 0);
  lv_obj_align(iconCircle, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_clear_flag(iconCircle, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_t* iconLbl = lv_label_create(iconCircle);
  lv_label_set_text(iconLbl, is_hashtag ? "#" : LV_SYMBOL_GPS);
  lv_obj_set_style_text_color(iconLbl, lv_color_white(), 0);
  lv_obj_set_style_text_font(iconLbl, &lv_font_montserrat_20, 0);
  lv_obj_center(iconLbl);

  lv_obj_t* lbl = lv_label_create(card);
  lv_label_set_text(lbl, e.name);
  lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
  lv_obj_set_width(lbl, SCR_W - 200);
  lv_obj_set_style_text_color(lbl,
    ch_muted ? lv_color_hex(g_theme->btn_danger) : lv_color_hex(g_theme->hs_channel_text), 0);
  lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
  lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 56, 0);

  if (unread > 0) {
    lv_obj_t* badge = lv_obj_create(card);
    lv_obj_set_size(badge, 32, 32);
  lv_obj_set_style_radius(badge, 0, 0);
    lv_obj_set_style_bg_color(badge, lv_color_hex(g_theme->hs_badge_bg), 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
    lv_obj_set_style_border_opa(badge, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(badge, 0, 0);
    lv_obj_align(badge, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t* blbl = lv_label_create(badge);
    char nbuf[8]; snprintf(nbuf, sizeof(nbuf), "%u", (unsigned)unread);
    lv_label_set_text(blbl, nbuf);
    lv_obj_set_style_text_color(blbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(blbl, &lv_font_montserrat_14, 0);
    lv_obj_center(blbl);
  }
}

// ---- Create contact bubble ----
static void create_contact_bubble(const DropdownEntry& e, uint16_t unread) {
  if (!ui_chatpanel || g_bubble_pool_count >= MAX_DD_ENTRIES * 2) return;
  BubbleTapData* td = &g_bubble_pool[g_bubble_pool_count++];
  td->kind = TargetKind::CONTACT; td->channel_idx = -1;
  memcpy(td->pub_key, e.contact_id.pub_key, 32);

  static const uint32_t AVATAR_PALETTE[] = {
    0x2563EB, 0x7C3AED, 0xDB2777, 0x059669,
    0xEA580C, 0x16A34A, 0x4F46E5, 0x0891B2,
  };
  uint8_t hash = 0;
  for (const char* p = e.name; *p; p++) hash = hash * 31 + (uint8_t)*p;
  uint32_t avatarColor = AVATAR_PALETTE[hash & 7];

  lv_obj_t* card = lv_obj_create(ui_chatpanel);
  lv_obj_set_width(card, lv_pct(100));
  lv_obj_set_height(card, 76);
  lv_obj_set_style_radius(card, 0, 0);
  lv_obj_set_style_bg_color(card, lv_color_hex(g_theme->hs_contact_bg), 0);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_border_color(card, lv_color_hex(g_theme->hs_contact_border), 0);
  lv_obj_set_style_border_opa(card, LV_OPA_COVER, 0);
  lv_obj_set_style_shadow_opa(card, LV_OPA_TRANSP, 0);
  lv_obj_set_style_pad_left(card, 10, 0);
  lv_obj_set_style_pad_right(card, 10, 0);
  lv_obj_set_style_pad_ver(card, 0, 0);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(card, cb_bubble_tapped,       LV_EVENT_CLICKED,       td);
  lv_obj_add_event_cb(card, cb_bubble_long_pressed, LV_EVENT_LONG_PRESSED,  td);

  lv_obj_t* avatar = lv_obj_create(card);
  lv_obj_set_size(avatar, 48, 48);
  lv_obj_set_style_radius(avatar, 0, 0);
  lv_obj_set_style_bg_color(avatar, lv_color_hex(avatarColor), 0);
  lv_obj_set_style_bg_opa(avatar, LV_OPA_COVER, 0);
  lv_obj_set_style_border_opa(avatar, LV_OPA_TRANSP, 0);
  lv_obj_set_style_pad_all(avatar, 0, 0);
  lv_obj_align(avatar, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_clear_flag(avatar, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  char initial[4] = {0};
  initial[0] = (e.name[0] >= 'a' && e.name[0] <= 'z') ? e.name[0] - 32 : e.name[0];
  lv_obj_t* avatarLbl = lv_label_create(avatar);
  lv_label_set_text(avatarLbl, initial);
  lv_obj_set_style_text_color(avatarLbl, lv_color_white(), 0);
  lv_obj_set_style_text_font(avatarLbl, &lv_font_montserrat_22, 0);
  lv_obj_center(avatarLbl);

  lv_obj_t* nameLbl = lv_label_create(card);
  lv_label_set_text(nameLbl, e.name);
  lv_label_set_long_mode(nameLbl, LV_LABEL_LONG_DOT);
  lv_obj_set_width(nameLbl, SCR_W - 220);
  lv_obj_set_style_text_color(nameLbl, lv_color_hex(g_theme->hs_contact_name), 0);
  lv_obj_set_style_text_font(nameLbl, &lv_font_montserrat_18, 0);
  lv_obj_set_pos(nameLbl, 64, 12);

  int8_t snr = snr_contact_get(e.contact_id.pub_key);
  int bars = snr_to_bars(snr);
  char info_line[64] = "";
  int pos = 0;
  if (snr != -128) {
    const char* bar_chars[] = {"____", "|___", "||__", "|||_", "||||"};
    pos += snprintf(info_line + pos, sizeof(info_line) - pos, "%s  ", bar_chars[bars]);
  }
  if (e.last_advert_ts > 0) {
    uint32_t tl = e.last_advert_ts + (uint32_t)DISPLAY_UTC_OFFSET_S;
    pos += snprintf(info_line + pos, sizeof(info_line) - pos, "%02lu:%02lu",
                    (unsigned long)((tl%86400)/3600), (unsigned long)((tl%3600)/60));
  }
  if (info_line[0]) {
    lv_obj_t* infoLbl = lv_label_create(card);
    lv_label_set_text(infoLbl, info_line);
    lv_obj_set_style_text_color(infoLbl, lv_color_hex(g_theme->hs_contact_seen), 0);
    lv_obj_set_style_text_font(infoLbl, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(infoLbl, 64, 42);
  }

  if (unread > 0) {
    lv_obj_t* badge = lv_obj_create(card);
    lv_obj_set_size(badge, 32, 32);
  lv_obj_set_style_radius(badge, 0, 0);
    lv_obj_set_style_bg_color(badge, lv_color_hex(g_theme->hs_badge_bg), 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
    lv_obj_set_style_border_opa(badge, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(badge, 0, 0);
    lv_obj_align(badge, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t* blbl = lv_label_create(badge);
    char nbuf[8]; snprintf(nbuf, sizeof(nbuf), "%u", (unsigned)unread);
    lv_label_set_text(blbl, nbuf);
    lv_obj_set_style_text_color(blbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(blbl, &lv_font_montserrat_14, 0);
    lv_obj_center(blbl);
  }
}

// ---- Build homescreen list ----

// Cheap 32-bit hash of the state that determines the rendered list:
// channel set + order, contact set + order, per-entry unread counts,
// search filter. FNV-1a-ish; collisions are fine because a miss just
// means we redraw when we didn't need to, not the other way round.
static uint32_t homescreen_state_hash() {
  uint32_t h = 2166136261u;
  auto mix = [&](uint32_t v) { h ^= v; h *= 16777619u; };

  mix((uint32_t)dd_channels_count);
  for (int i = 0; i < dd_channels_count; i++) {
    mix((uint32_t)dd_channels[i].channel_idx);
    const char* p = dd_channels[i].name;
    while (*p) { mix((uint8_t)*p++); }
    if (dd_channels[i].channel_idx >= 0 && dd_channels[i].channel_idx < 32)
      mix(notify_channel_get(dd_channels[i].channel_idx));
    mix(g_muted_channel_mask);
  }

  mix((uint32_t)dd_contacts_count);
  for (int i = 0; i < dd_contacts_count; i++) {
    const uint8_t* k = dd_contacts[i].contact_id.pub_key;
    mix(((uint32_t)k[0] << 24) | ((uint32_t)k[1] << 16) |
        ((uint32_t)k[2] <<  8) |  (uint32_t)k[3]);
    mix(dd_contacts[i].last_advert_ts);
    mix(notify_contact_get(k));
    const char* p = dd_contacts[i].name;
    while (*p) { mix((uint8_t)*p++); }
  }

  const char* p = g_search_filter;
  while (*p) { mix((uint8_t)*p++); }
  return h;
}

void build_homescreen_list() {
  if (!ui_chatpanel) return;

  // Idempotent fast path: if the list contents (channels, contacts,
  // unread counts, mute mask, search filter) are bit-identical to what
  // we rendered last time, AND the panel is currently showing a home
  // list (not chat bubbles from an in-progress chat exit), skip the
  // whole lv_obj_clean + widget-create cycle. Messages flowing in
  // during an idle channel fire this a lot via the main-loop
  // g_deferred_refresh_targets flag; without the skip, every poll
  // rebuilds a handful of identical bubbles.
  //
  // The `g_homescreen_list_is_current` flag is cleared by chat_add()
  // and chat_clear() whenever they populate the panel with non-home
  // content, so a rebuild always fires on exit_chat_mode even when
  // the underlying contact/channel state is unchanged — without it,
  // swipe-back from a chat found the hash unchanged, skipped the
  // rebuild, and left the chat bubbles on-screen.
  static uint32_t s_last_hash = 0;
  static bool     s_have_hash = false;
  uint32_t h = homescreen_state_hash();
  if (s_have_hash && h == s_last_hash &&
      g_homescreen_list_is_current &&
      lv_obj_get_child_cnt(ui_chatpanel) > 1) {
    return;
  }
  s_last_hash = h;
  s_have_hash = true;
  g_homescreen_list_is_current = true;

  g_bubble_pool_count = 0; g_scroll_btn = nullptr;
  g_pending_status_label = nullptr;
  g_deferred_status_char = 0;
  g_channel_receipt.status_label = nullptr;
  for (int i = 0; i < PM_RING_SIZE; i++) g_pm_ring[i].status_label = nullptr;
  g_del = {};
  // See comment in chat_clear() — the NEW-MESSAGES divider (if any)
  // lives under ui_chatpanel, so lv_obj_clean invalidates it. Zero the
  // global pointer first so the main-loop deferred handler can't race
  // and deref freed memory.
  g_pending_scroll_to_divider = nullptr;
  lv_obj_clean(ui_chatpanel);
  lv_obj_set_style_pad_top(ui_chatpanel, 8, 0);

  for (int i = 0; i < dd_channels_count; i++) {
    if (g_search_filter[0]) {
      char lower[32]; strncpy(lower, dd_channels[i].name, 31); lower[31] = '\0';
      for (char* p = lower; *p; p++) *p = tolower((unsigned char)*p);
      if (!strstr(lower, g_search_filter)) continue;
    }
    create_channel_bubble(dd_channels[i], notify_channel_get(dd_channels[i].channel_idx));
  }

  int order[MAX_DD_ENTRIES];
  int visible = 0;
  for (int i = 0; i < dd_contacts_count; i++) {
    if (g_search_filter[0]) {
      char lower[32]; strncpy(lower, dd_contacts[i].name, 31); lower[31] = '\0';
      for (char* p = lower; *p; p++) *p = tolower((unsigned char)*p);
      if (!strstr(lower, g_search_filter)) continue;
    }
    order[visible++] = i;
  }
  for (int i = 1; i < visible; i++) {
    int ki = order[i];
    uint16_t ku = notify_contact_get(dd_contacts[ki].contact_id.pub_key);
    uint32_t kts = dd_contacts[ki].last_advert_ts;
    int j = i - 1;
    while (j >= 0) {
      int kj = order[j];
      uint16_t ou = notify_contact_get(dd_contacts[kj].contact_id.pub_key);
      uint32_t ots = dd_contacts[kj].last_advert_ts;
      bool sw = (ku > 0 && ou == 0) || (ku > 0 && ou > 0 && kts > ots) || (ku == 0 && ou == 0 && kts > ots);
      if (!sw) break;
      order[j+1] = order[j]; j--;
    }
    order[j+1] = ki;
  }
  for (int i = 0; i < visible; i++) {
    int idx = order[i];
    create_contact_bubble(dd_contacts[idx], notify_contact_get(dd_contacts[idx].contact_id.pub_key));
  }
  scroll_btn_ensure();
  // Cold-boot fix: force LVGL to re-measure the flex column right
  // now so the panel's internal scroll extent reflects the bubbles
  // we just added. Without this, on first build_homescreen_list()
  // after boot, the extent stays 0 and touch drags can't scroll —
  // the user has to enter + exit a chat (which toggles unrelated
  // widgets' visibility and indirectly forces a layout pass) before
  // scrolling starts working.
  lv_obj_update_layout(ui_chatpanel);
}

// ---- ui_refresh_targets ----
void ui_refresh_targets() {
  dd_contacts_count = 0;
  dd_channels_count = 0;
  if (!g_mesh) return;

  ChannelDetails ch;
  for (int i=0; i<MAX_GROUP_CHANNELS && dd_channels_count < MAX_DD_ENTRIES; i++) {
    if (!g_mesh->getChannel(i, ch)) break;
    if (ch.name[0] == 0) break;
    if (i < 32 && (g_deleted_channel_mask & (1u << i))) continue;
    dd_channels[dd_channels_count].kind = TargetKind::CHANNEL;
    dd_channels[dd_channels_count].channel_idx = i;
    dd_channels[dd_channels_count].last_advert_ts = 0;
    snprintf(dd_channels[dd_channels_count].name, sizeof(dd_channels[dd_channels_count].name), "%s", ch.name);
    sanitize_ascii_inplace(dd_channels[dd_channels_count].name);
    dd_channels_count++;
  }

  ContactInfo c;
  for (uint32_t i=0; i<(uint32_t)g_mesh->getNumContacts(); i++) {
    if (!g_mesh->getContactByIdx(i, c)) continue;
    if (c.name[0] == 0 || (c.flags & CONTACT_FLAG_HIDDEN)) continue;
    if (c.type == ADV_TYPE_REPEATER) continue;

    char nm[32];
    StrHelper::strncpy(nm, c.name, sizeof(nm));
    sanitize_ascii_inplace(nm);

    if (dd_contacts_count < MAX_DD_ENTRIES) {
      dd_contacts[dd_contacts_count].kind = TargetKind::CONTACT;
      dd_contacts[dd_contacts_count].contact_id = c.id;
      dd_contacts[dd_contacts_count].last_advert_ts = c.last_advert_timestamp;
      snprintf(dd_contacts[dd_contacts_count].name, sizeof(dd_contacts[dd_contacts_count].name), "%s", nm);
      sanitize_ascii_inplace(dd_contacts[dd_contacts_count].name);
      dd_contacts_count++;
    }
  }

  if (!g_in_chat_mode) build_homescreen_list();
}

// ---- Enter / Exit chat mode ----
void enter_chat_mode(TargetKind kind, int ch_idx, const uint8_t* pub_key) {
  g_in_chat_mode = true;
  g_search_filter[0] = '\0';
  if (ui_searchfield) {
    lv_textarea_set_text(ui_searchfield, "");
    lv_obj_add_flag(ui_searchfield, LV_OBJ_FLAG_HIDDEN);
  }
  if (ui_backbutton) lv_obj_clear_flag(ui_backbutton, LV_OBJ_FLAG_HIDDEN);
  if (g_chat_header_label) { lv_obj_del(g_chat_header_label); g_chat_header_label = nullptr; }
  g_chat_target_name[0] = '\0';

  if (ui_mutebutton) {
    if (kind == TargetKind::CHANNEL) {
      lv_obj_clear_flag(ui_mutebutton, LV_OBJ_FLAG_HIDDEN);
      ui_apply_mute_button_state();
    } else {
      lv_obj_add_flag(ui_mutebutton, LV_OBJ_FLAG_HIDDEN);
    }
  }

  if (kind == TargetKind::CHANNEL && g_mesh) {
    mesh_select_channel_by_idx(ch_idx);
  } else if (kind == TargetKind::CONTACT && g_mesh && pub_key) {
    mesh_select_contact_by_pubkey(pub_key);
  }

  ui_apply_mute_button_state();
  // Text field starts in its idle position (just above the tab bar).
  // The keyboard stays hidden on chat entry — it only appears when the
  // user taps the text field (cb_textsend_focused moves the field up
  // to KB_TA_Y and shows the keyboard then). This matches the "one-tap
  // extra for typing, no keyboard obstructing a quick-read chat open"
  // behaviour requested against the earlier auto-show-on-entry design.
  if (ui_textsendtype) {
    lv_obj_clear_flag(ui_textsendtype, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_y(ui_textsendtype, TEXTSEND_Y_DEFAULT);
  }
  // Wire the keyboard to the text field so once the user taps and the
  // keyboard appears, typing routes to ui_textsendtype without a
  // second setup step — but keep it hidden until that tap happens.
  if (ui_Keyboard1 && ui_textsendtype) {
    lv_keyboard_set_textarea(ui_Keyboard1, ui_textsendtype);
    lv_obj_add_flag(ui_Keyboard1, LV_OBJ_FLAG_HIDDEN);
  }
  // Respect the "scroll-to-NEW-MESSAGES-divider" positioning done inside
  // load_chat_from_file() when there are unread messages. Without this
  // suppression, the main loop's deferred handler fires a few ms later
  // and overrides the divider scroll with chat_scroll_to_newest().
  if (g_suppress_next_scroll_bottom) {
    g_suppress_next_scroll_bottom = false;
  } else {
    g_deferred_scroll_bottom = true;
  }
}

void exit_chat_mode() {
  g_in_chat_mode = false;
  // Drop the active chat key so any translation completing after this
  // point writes to the file but doesn't touch the chatpanel widgets
  // (which belong to whatever screen the user just switched to).
  g_current_chat_key[0] = '\0';
  kb_hide(ui_Keyboard1, ui_textsendtype);
  if (ui_textsendtype) {
    lv_obj_set_y(ui_textsendtype, TEXTSEND_Y_DEFAULT);
    lv_obj_add_flag(ui_textsendtype, LV_OBJ_FLAG_HIDDEN);
  }
  if (ui_backbutton) lv_obj_add_flag(ui_backbutton, LV_OBJ_FLAG_HIDDEN);
  if (ui_mutebutton) lv_obj_add_flag(ui_mutebutton, LV_OBJ_FLAG_HIDDEN);
  if (g_chat_header_label) { lv_obj_del(g_chat_header_label); g_chat_header_label = nullptr; }
  g_chat_target_name[0] = '\0';
  if (ui_chatpanel) lv_obj_set_style_pad_top(ui_chatpanel, 8, 0);
  if (ui_searchfield) {
    lv_obj_clear_flag(ui_searchfield, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(ui_searchfield, 0, SEARCH_Y_OFFSET);
  }
  build_homescreen_list();
}
