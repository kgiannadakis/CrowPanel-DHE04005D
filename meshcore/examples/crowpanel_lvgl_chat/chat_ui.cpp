// ============================================================
// chat_ui.cpp — Chat screen rendering (bubbles, header, scroll)
// ============================================================

#include "chat_ui.h"
#include "utils.h"
#include "app_globals.h"
#include "mesh_api.h"
#include "translate.h"

#include "ui.h"
#include "ui_homescreen.h"

static lv_obj_t* g_path_reset_btn = nullptr;
static lv_obj_t* g_path_discover_btn = nullptr;

// ---- Long-press translate callback ----
extern bool g_wifi_connected;

static void cb_bubble_long_press_translate(lv_event_t* e) {
    if (g_auto_translate_enabled) return;  // auto-translate handles it
    if (!g_wifi_connected) return;
    // user_data is the bubble container, target is the label that was touched
    lv_obj_t* bubble = (lv_obj_t*)lv_event_get_user_data(e);
    lv_obj_t* touched = lv_event_get_target(e);
    if (!bubble || !touched) return;
    // Read body text from the touched label (or find it in the bubble)
    const char* body = lv_label_get_text(touched);
    if (body && body[0]) {
        translate_request(body, bubble);
    }
}

// ---- Chat rendering ----
lv_obj_t* chat_add(bool out, const char* txt, bool live, char loaded_status, const char* signal_info, uint16_t loaded_repeat_count, lv_obj_t** bubble_out, const char* translation) {
  if (!ui_chatpanel || !txt) return nullptr;

  String s = sanitize_ascii_string(txt);

  String ts = "";
  if (s.startsWith("[")) {
    int rb = s.indexOf(']');
    if (rb > 0 && rb < 16) {
      ts = s.substring(1, rb);
      int cut = rb + 1;
      if (cut < (int)s.length() && s[cut] == ' ') cut++;
      s = s.substring(cut);
    }
  }

  int sep = s.indexOf(": ");
  String name = (sep > 0) ? s.substring(0, sep + 1) : (out ? String("Me:") : String(""));
  String body = (sep > 0) ? s.substring(sep + 2) : s;
  String safeBody = sanitize_for_font_string(body.c_str(), &lv_font_montserrat_16);

  lv_obj_t* bubble = lv_obj_create(ui_chatpanel);
  int bubble_pct = g_landscape_mode ? 65 : 82;
  int shift_pct  = g_landscape_mode ? 35 : 18;
  lv_obj_set_width(bubble, lv_pct(bubble_pct));
  lv_obj_set_height(bubble, LV_SIZE_CONTENT);
  lv_obj_set_style_radius(bubble, 16, 0);
  lv_obj_set_style_bg_color(bubble, lv_color_hex(out ? g_theme->bubble_out : g_theme->bubble_in), 0);
  lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
  lv_obj_set_style_border_opa(bubble, LV_OPA_TRANSP, 0);
  lv_obj_set_style_shadow_opa(bubble, LV_OPA_TRANSP, 0);
  lv_obj_set_style_pad_left(bubble, 12, 0);
  lv_obj_set_style_pad_right(bubble, 12, 0);
  lv_obj_set_style_pad_top(bubble, 8, 0);
  lv_obj_set_style_pad_bottom(bubble, 6, 0);
  lv_obj_set_style_pad_row(bubble, 2, 0);
  lv_obj_set_flex_flow(bubble, LV_FLEX_FLOW_COLUMN);
  lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);

  if (out) {
    lv_obj_set_style_translate_x(bubble, lv_pct(shift_pct), 0);
  }

  String safeName = sanitize_for_font_string(name.c_str(), &lv_font_montserrat_14);
  safeName.replace("#", "");
  String safeTs = ts;
  safeTs.replace("#", "");

  // Build signal info string for header (hops + SNR, shown next to sender)
  String safeSig = "";
  if (!out && signal_info && signal_info[0]) {
    safeSig = sanitize_for_font_string(signal_info, &lv_font_montserrat_14);
    safeSig.replace("#", "");
  }

  if (safeTs.length() || safeName.length()) {
    char header[300];
    if (safeTs.length() && safeName.length() && safeSig.length()) {
      snprintf(header, sizeof(header), "#%06lX %s#  #%06lX %s#  #%06lX %s#",
               (unsigned long)g_theme->bubble_ts, safeTs.c_str(),
               (unsigned long)g_theme->bubble_name, safeName.c_str(),
               (unsigned long)g_theme->signal_text, safeSig.c_str());
    } else if (safeTs.length() && safeSig.length()) {
      snprintf(header, sizeof(header), "#%06lX %s#  #%06lX %s#",
               (unsigned long)g_theme->bubble_ts, safeTs.c_str(),
               (unsigned long)g_theme->signal_text, safeSig.c_str());
    } else if (safeName.length() && safeSig.length()) {
      snprintf(header, sizeof(header), "#%06lX %s#  #%06lX %s#",
               (unsigned long)g_theme->bubble_name, safeName.c_str(),
               (unsigned long)g_theme->signal_text, safeSig.c_str());
    } else if (safeTs.length() && safeName.length()) {
      snprintf(header, sizeof(header), "#%06lX %s#  #%06lX %s#",
               (unsigned long)g_theme->bubble_ts, safeTs.c_str(),
               (unsigned long)g_theme->bubble_name, safeName.c_str());
    } else if (safeName.length()) {
      snprintf(header, sizeof(header), "#%06lX %s#",
               (unsigned long)g_theme->bubble_name, safeName.c_str());
    } else {
      snprintf(header, sizeof(header), "#%06lX %s#",
               (unsigned long)g_theme->bubble_ts, safeTs.c_str());
    }
    lv_obj_t* hdrLbl = lv_label_create(bubble);
    lv_label_set_recolor(hdrLbl, true);
    lv_label_set_text(hdrLbl, header);
    lv_obj_set_width(hdrLbl, lv_pct(100));
    lv_obj_set_style_text_color(hdrLbl, lv_color_hex(g_theme->bubble_text), 0);
    lv_obj_set_style_text_font(hdrLbl, &lv_font_montserrat_14, 0);
  }

  lv_obj_t* lbl = lv_label_create(bubble);
  lv_label_set_recolor(lbl, false);
  lv_label_set_text(lbl, safeBody.c_str());
  lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(lbl, lv_pct(100));
  lv_obj_set_style_text_color(lbl, lv_color_hex(g_theme->bubble_text), 0);
  lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_line_space(lbl, 3, 0);

  lv_obj_t* lblStatus = nullptr;
  if (out && live) {
    if (g_pending_status_label) {
      lv_label_set_text(g_pending_status_label, LV_SYMBOL_WARNING " unconfirmed");
      lv_obj_set_style_text_color(g_pending_status_label, lv_color_hex(g_theme->status_pending), 0);
      lv_obj_set_style_text_opa(g_pending_status_label, LV_OPA_COVER, 0);
      g_pending_status_label = nullptr;
    }
    lblStatus = lv_label_create(bubble);
    lv_label_set_text(lblStatus, "Sending");
    lv_obj_set_style_text_color(lblStatus, lv_color_hex(g_theme->bubble_text), 0);
    lv_obj_set_style_text_opa(lblStatus, LV_OPA_50, 0);
    lv_obj_set_style_text_font(lblStatus, &lv_font_montserrat_12, 0);
    // Caller stores this label where appropriate (ring entry for PMs, g_pending_status_label for channels)
  } else if (out && loaded_status != 0) {
    lblStatus = lv_label_create(bubble);
    lv_obj_set_style_text_font(lblStatus, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_opa(lblStatus, LV_OPA_COVER, 0);
    if (loaded_status == 'D') {
      lv_label_set_text(lblStatus, LV_SYMBOL_OK " delivered");
      lv_obj_set_style_text_color(lblStatus, lv_color_hex(g_theme->status_delivered), 0);
    } else if (loaded_status == 'U') {
      lv_label_set_text(lblStatus, LV_SYMBOL_WARNING " Unknown, acknowledge will fail in 1'");
      lv_obj_set_style_text_color(lblStatus, lv_color_hex(g_theme->status_pending), 0);
    } else if (loaded_status == 'N') {
      lv_label_set_text(lblStatus, "Failed");
      lv_obj_set_style_text_color(lblStatus, lv_color_hex(g_theme->status_failed), 0);
    } else if (loaded_status == 'R') {
      if (loaded_repeat_count == 0) {
        lv_label_set_text(lblStatus, LV_SYMBOL_OK " Sent");
      } else {
        char rbuf[48];
        snprintf(rbuf, sizeof(rbuf), LV_SYMBOL_OK " Heard %u repeater%s",
                 (unsigned)loaded_repeat_count, loaded_repeat_count == 1 ? "" : "s");
        lv_label_set_text(lblStatus, rbuf);
      }
      lv_obj_set_style_text_color(lblStatus, lv_color_white(), 0);
    } else {
      lv_label_set_text(lblStatus, LV_SYMBOL_WARNING " unconfirmed");
      lv_obj_set_style_text_color(lblStatus, lv_color_hex(g_theme->status_pending), 0);
    }
  }

  // Signal info (hops + SNR) is now shown in the header next to sender name

  // Long-press body label to translate incoming bubbles.
  // Register on the label (it receives touch events), pass bubble as user_data.
  if (!out && lbl) {
    lv_obj_add_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(lbl, cb_bubble_long_press_translate, LV_EVENT_LONG_PRESSED, bubble);
  }

  // Show inline translation if provided (from file history or auto-translate)
  if (translation && translation[0]) {
    String safeTranslation = sanitize_for_font_string(translation, &lv_font_montserrat_12);
    lv_obj_t* trans_lbl = lv_label_create(bubble);
    lv_label_set_long_mode(trans_lbl, LV_LABEL_LONG_WRAP);
    lv_label_set_text(trans_lbl, safeTranslation.c_str());
    lv_obj_set_width(trans_lbl, lv_pct(100));
    lv_obj_set_style_text_color(trans_lbl, lv_color_hex(TH_TEXT3), 0);
    lv_obj_set_style_text_font(trans_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_pad_top(trans_lbl, 4, 0);
  }

  // Expose the bubble to caller if requested
  if (bubble_out) *bubble_out = bubble;

  if (!g_loading_history) chat_scroll_to_newest();
  return lblStatus;
}

// ---- Status helpers ----
void apply_status_to_label(lv_obj_t* lbl, char sc) {
  if (!lbl) return;
  if (sc == 'D') {
    lv_label_set_text(lbl, LV_SYMBOL_OK " delivered");
    lv_obj_set_style_text_color(lbl, lv_color_hex(g_theme->status_delivered), 0);
  } else if (sc == 'R') {
    lv_label_set_text(lbl, LV_SYMBOL_OK " Sent");
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
  } else if (sc == 'U') {
    lv_label_set_text(lbl, LV_SYMBOL_WARNING " Unknown, acknowledge will fail in 1'");
    lv_obj_set_style_text_color(lbl, lv_color_hex(g_theme->status_pending), 0);
  } else if (sc == 'N') {
    lv_label_set_text(lbl, "Failed");
    lv_obj_set_style_text_color(lbl, lv_color_hex(g_theme->status_failed), 0);
  } else {
    lv_label_set_text(lbl, LV_SYMBOL_WARNING " unconfirmed");
    lv_obj_set_style_text_color(lbl, lv_color_hex(g_theme->status_pending), 0);
  }
  lv_obj_set_style_text_opa(lbl, LV_OPA_COVER, 0);
}

void apply_receipt_count_to_label(lv_obj_t* lbl, uint16_t count, int8_t snr_raw) {
  if (!lbl) return;
  if (count == 0) {
    lv_label_set_text(lbl, LV_SYMBOL_OK " Sent");
  } else {
    char text[48];
    snprintf(text, sizeof(text), LV_SYMBOL_OK " Heard %u repeater%s",
             (unsigned)count, count == 1 ? "" : "s");
    lv_label_set_text(lbl, text);
  }
  lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
  lv_obj_set_style_text_opa(lbl, LV_OPA_COVER, 0);
}

// ---- Resend button ----

struct ResendData {
  uint8_t pub_key[32];
  char    text[241];
  lv_obj_t* status_label;  // to restore the pending PM label while retrying
};

static void cb_resend_btn_delete(lv_event_t* e) {
  // Free the ResendData when the button is deleted
  ResendData* rd = (ResendData*)lv_event_get_user_data(e);
  if (rd) lv_mem_free(rd);
}

static void cb_resend_pressed(lv_event_t* e) {
  ResendData* rd = (ResendData*)lv_event_get_user_data(e);
  if (!rd) return;

  // Re-transmit using the same ring slot / bubble / file entry. The slot's
  // ui_dirty flag flows through the main-loop label updater to set the
  // correct "Sending" text automatically — no manual label mutation needed.
  bool ok = mesh_resend_pm_by_bubble_label(rd->status_label);
  serialmon_append(ok ? "Resend triggered" : "Resend failed: no matching slot");

  // Remove the resend button (this triggers cb_resend_btn_delete → frees rd)
  lv_obj_t* btn = lv_event_get_target(e);
  if (btn) lv_obj_del(btn);
}

void chat_add_resend_btn(lv_obj_t* status_label, const uint8_t* pub_key, const char* text) {
  if (!status_label || !pub_key || !text || !text[0]) return;

  lv_obj_t* bubble = lv_obj_get_parent(status_label);
  if (!bubble) return;

  // Allocate resend data
  ResendData* rd = (ResendData*)lv_mem_alloc(sizeof(ResendData));
  if (!rd) return;
  memcpy(rd->pub_key, pub_key, 32);
  strncpy(rd->text, text, sizeof(rd->text) - 1);
  rd->text[sizeof(rd->text) - 1] = '\0';
  rd->status_label = status_label;

  // Create button inside the bubble
  lv_obj_t* btn = lv_btn_create(bubble);
  lv_obj_set_size(btn, LV_SIZE_CONTENT, 32);
  lv_obj_set_style_radius(btn, 10, 0);
  lv_obj_set_style_bg_color(btn, lv_color_hex(g_theme->btn_active), 0);
  lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_left(btn, 12, 0);
  lv_obj_set_style_pad_right(btn, 12, 0);
  lv_obj_set_style_pad_top(btn, 4, 0);
  lv_obj_set_style_pad_bottom(btn, 4, 0);

  lv_obj_t* lbl = lv_label_create(btn);
  lv_label_set_text(lbl, LV_SYMBOL_LOOP " Resend");
  lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(lbl, lv_color_white(), 0);

  lv_obj_add_event_cb(btn, cb_resend_pressed, LV_EVENT_CLICKED, rd);
  lv_obj_add_event_cb(btn, cb_resend_btn_delete, LV_EVENT_DELETE, rd);
}

// ---- Chat scroll ----
void chat_scroll_to_newest() {
  if (!ui_chatpanel) return;

  bool kb_active = g_in_chat_mode && ui_Keyboard1 &&
                   !lv_obj_has_flag(ui_Keyboard1, LV_OBJ_FLAG_HIDDEN);

  if (kb_active) {
    lv_obj_update_layout(lv_scr_act());
    lv_area_t kb_area;
    lv_obj_get_coords(ui_Keyboard1, &kb_area);
    lv_coord_t kb_top = kb_area.y1;
    lv_area_t ta_area;
    lv_obj_get_coords(ui_textsendtype, &ta_area);
    lv_coord_t obstacle_top = (ta_area.y1 < kb_top) ? ta_area.y1 : kb_top;
    static const lv_coord_t CLEARANCE = 16;
    lv_coord_t target_bottom = obstacle_top - CLEARANCE;
    lv_coord_t display_h  = lv_disp_get_ver_res(lv_disp_get_default());
    lv_coord_t viewport_h = display_h - g_chatpanel_orig_y;
    lv_coord_t pad_bottom = (g_chatpanel_orig_y + viewport_h) - target_bottom;
    if (pad_bottom < 8) pad_bottom = 8;
    lv_obj_set_style_pad_bottom(ui_chatpanel, pad_bottom, 0);
    lv_obj_update_layout(ui_chatpanel);
  } else if (g_in_chat_mode && ui_textsendtype &&
             !lv_obj_has_flag(ui_textsendtype, LV_OBJ_FLAG_HIDDEN)) {
    lv_obj_update_layout(lv_scr_act());
    lv_area_t ta_area;
    lv_obj_get_coords(ui_textsendtype, &ta_area);
    lv_coord_t display_h  = lv_disp_get_ver_res(lv_disp_get_default());
    lv_coord_t viewport_h = display_h - g_chatpanel_orig_y;
    lv_coord_t pad_bottom = (g_chatpanel_orig_y + viewport_h) - (ta_area.y1 - 8);
    if (pad_bottom < 8) pad_bottom = 8;
    lv_obj_set_style_pad_bottom(ui_chatpanel, pad_bottom, 0);
  } else {
    lv_obj_set_style_pad_bottom(ui_chatpanel, 16, 0);
  }

  lv_obj_scroll_to_y(ui_chatpanel, LV_COORD_MAX, LV_ANIM_OFF);
}

// ---- Scroll-to-newest FAB ----
void cb_scroll_to_newest(lv_event_t*) {
  chat_scroll_to_newest();
}

void cb_chatpanel_scroll(lv_event_t*) {
  if (!g_scroll_btn || !ui_chatpanel) return;
  if (lv_obj_get_scroll_bottom(ui_chatpanel) > 80)
    lv_obj_clear_flag(g_scroll_btn, LV_OBJ_FLAG_HIDDEN);
  else
    lv_obj_add_flag(g_scroll_btn, LV_OBJ_FLAG_HIDDEN);
}

void scroll_btn_ensure() {
  if (!ui_chatpanel) return;
  g_scroll_btn = lv_btn_create(ui_chatpanel);
  lv_obj_add_flag(g_scroll_btn, LV_OBJ_FLAG_FLOATING);
  lv_obj_set_size(g_scroll_btn, 40, 40);
  lv_obj_set_style_radius(g_scroll_btn, 20, 0);
  lv_obj_set_style_bg_color(g_scroll_btn, lv_color_hex(g_theme->scroll_btn_bg), 0);
  lv_obj_set_style_bg_opa(g_scroll_btn, LV_OPA_90, 0);
  lv_obj_set_style_border_opa(g_scroll_btn, LV_OPA_TRANSP, 0);
  lv_obj_set_style_shadow_opa(g_scroll_btn, LV_OPA_TRANSP, 0);
  lv_obj_set_style_pad_all(g_scroll_btn, 0, 0);
  lv_obj_align(g_scroll_btn, LV_ALIGN_BOTTOM_RIGHT, -12, -12);
  lv_obj_add_flag(g_scroll_btn, LV_OBJ_FLAG_HIDDEN);
  lv_obj_t* lbl = lv_label_create(g_scroll_btn);
  lv_label_set_text(lbl, LV_SYMBOL_DOWN);
  lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
  lv_obj_center(lbl);
  lv_obj_add_event_cb(g_scroll_btn, cb_scroll_to_newest, LV_EVENT_CLICKED, nullptr);
}

// ---- Chat header ----
void chat_update_route_label() {
  if (!g_chat_route_label) return;
  if (!g_chat_route_is_contact) {
    lv_label_set_text(g_chat_route_label, "");
    if (g_path_reset_btn) lv_obj_add_flag(g_path_reset_btn, LV_OBJ_FLAG_HIDDEN);
    if (g_path_discover_btn) lv_obj_add_flag(g_path_discover_btn, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  if (g_chat_route_path_len == OUT_PATH_UNKNOWN) {
    lv_label_set_text(g_chat_route_label, "flood");
    lv_obj_set_style_text_color(g_chat_route_label, lv_color_hex(0xFFA040), 0);
    if (g_path_reset_btn) lv_obj_add_flag(g_path_reset_btn, LV_OBJ_FLAG_HIDDEN);
    if (g_path_discover_btn) lv_obj_clear_flag(g_path_discover_btn, LV_OBJ_FLAG_HIDDEN);
  } else if (g_chat_route_path_len == 0) {
    lv_label_set_text(g_chat_route_label, "direct");
    lv_obj_set_style_text_color(g_chat_route_label, lv_color_hex(0x6DC264), 0);
    if (g_path_reset_btn) lv_obj_clear_flag(g_path_reset_btn, LV_OBJ_FLAG_HIDDEN);
    if (g_path_discover_btn) lv_obj_add_flag(g_path_discover_btn, LV_OBJ_FLAG_HIDDEN);
  } else {
    char buf[16];
    uint8_t true_hops = g_chat_route_path_len & 63;  // decode: lower 6 bits = hop count
    snprintf(buf, sizeof(buf), "%u hop%s",
             (unsigned)true_hops,
             true_hops == 1 ? "" : "s");
    lv_label_set_text(g_chat_route_label, buf);
    lv_obj_set_style_text_color(g_chat_route_label, lv_color_hex(0x5EB5F7), 0);
    if (g_path_reset_btn) lv_obj_clear_flag(g_path_reset_btn, LV_OBJ_FLAG_HIDDEN);
    if (g_path_discover_btn) lv_obj_add_flag(g_path_discover_btn, LV_OBJ_FLAG_HIDDEN);
  }
}

void chat_set_header(const char* name) {
  if (!ui_chatpanel) return;

  if (name) {
    strncpy(g_chat_target_name, name, sizeof(g_chat_target_name) - 1);
    g_chat_target_name[sizeof(g_chat_target_name) - 1] = '\0';
  }

  if (g_chat_header_label) {
    lv_obj_del(g_chat_header_label);
    g_chat_header_label = nullptr;
    g_chat_route_label  = nullptr;
    g_path_reset_btn    = nullptr;
    g_path_discover_btn = nullptr;
  }

  if (!g_chat_target_name[0]) return;

  static const lv_coord_t HEADER_H = 48;

  lv_obj_t* parent = lv_obj_get_parent(ui_chatpanel);
  if (!parent) parent = lv_scr_act();

  g_chat_header_label = lv_obj_create(parent);
  lv_obj_set_size(g_chat_header_label, lv_obj_get_width(parent), HEADER_H);
  lv_obj_set_pos(g_chat_header_label, 0, STATUS_H);
  lv_obj_set_style_bg_color(g_chat_header_label, lv_color_hex(g_theme->chat_header_bg), 0);
  lv_obj_set_style_bg_opa(g_chat_header_label, LV_OPA_COVER, 0);
  lv_obj_set_style_border_opa(g_chat_header_label, LV_OPA_TRANSP, 0);
  lv_obj_set_style_radius(g_chat_header_label, 0, 0);
  lv_obj_set_style_pad_all(g_chat_header_label, 0, 0);
  lv_obj_clear_flag(g_chat_header_label, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* lbl = lv_label_create(g_chat_header_label);
  lv_label_set_text(lbl, g_chat_target_name);
  lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
  lv_obj_set_width(lbl, lv_obj_get_width(parent) - 100);
  lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_color(lbl, lv_color_hex(g_theme->chat_header_text), 0);
  lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
  lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -6);

  g_chat_route_label = lv_label_create(g_chat_header_label);
  lv_obj_set_style_text_font(g_chat_route_label, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_align(g_chat_route_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(g_chat_route_label, LV_ALIGN_CENTER, 0, 14);

  // "Reset path" button — shown for PM contacts when path is not flood
  g_path_reset_btn = lv_btn_create(g_chat_header_label);
  lv_obj_set_size(g_path_reset_btn, LV_SIZE_CONTENT, 22);
  lv_obj_set_style_pad_hor(g_path_reset_btn, 8, 0);
  lv_obj_set_style_pad_ver(g_path_reset_btn, 2, 0);
  lv_obj_set_style_bg_color(g_path_reset_btn, lv_color_hex(0x2B3B4D), 0);
  lv_obj_set_style_bg_opa(g_path_reset_btn, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(g_path_reset_btn, 4, 0);
  lv_obj_align(g_path_reset_btn, LV_ALIGN_RIGHT_MID, -6, 6);
  lv_obj_t* rlbl = lv_label_create(g_path_reset_btn);
  lv_label_set_text(rlbl, LV_SYMBOL_REFRESH " flood");
  lv_obj_set_style_text_font(rlbl, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(rlbl, lv_color_hex(0xFFA040), 0);
  lv_obj_center(rlbl);
  lv_obj_add_event_cb(g_path_reset_btn, [](lv_event_t*) {
    mesh_reset_current_contact_path();
  }, LV_EVENT_CLICKED, nullptr);
  lv_obj_add_flag(g_path_reset_btn, LV_OBJ_FLAG_HIDDEN);  // hidden by default

  // "Discover path" button — shown for PM contacts when in flood mode
  g_path_discover_btn = lv_btn_create(g_chat_header_label);
  lv_obj_set_size(g_path_discover_btn, LV_SIZE_CONTENT, 22);
  lv_obj_set_style_pad_hor(g_path_discover_btn, 8, 0);
  lv_obj_set_style_pad_ver(g_path_discover_btn, 2, 0);
  lv_obj_set_style_bg_color(g_path_discover_btn, lv_color_hex(0x2B3B4D), 0);
  lv_obj_set_style_bg_opa(g_path_discover_btn, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(g_path_discover_btn, 4, 0);
  lv_obj_align(g_path_discover_btn, LV_ALIGN_RIGHT_MID, -6, 6);
  lv_obj_t* dlbl = lv_label_create(g_path_discover_btn);
  lv_label_set_text(dlbl, LV_SYMBOL_GPS " discover");
  lv_obj_set_style_text_font(dlbl, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(dlbl, lv_color_hex(0x5EB5F7), 0);
  lv_obj_center(dlbl);
  lv_obj_add_event_cb(g_path_discover_btn, [](lv_event_t*) {
    mesh_discover_contact_path();
  }, LV_EVENT_CLICKED, nullptr);
  lv_obj_add_flag(g_path_discover_btn, LV_OBJ_FLAG_HIDDEN);  // hidden by default

  chat_update_route_label();

  // Pad top so bubbles start below the header (which is at STATUS_H)
  {
    lv_coord_t hp = (STATUS_H + HEADER_H) - g_chatpanel_orig_y + 4;
    if (hp < 4) hp = 4;
    lv_obj_set_style_pad_top(ui_chatpanel, hp, 0);
  }
}

// ---- Chat clear ----
void chat_clear() {
  g_scroll_btn = nullptr;
  if (!ui_chatpanel) return;
  lv_obj_clean(ui_chatpanel);
  if (!g_chat_target_name[0]) lv_obj_set_style_pad_top(ui_chatpanel, 8, 0);
  g_pending_status_label = nullptr;
  g_deferred_status_char = 0;
  g_channel_receipt.status_label = nullptr;
  // Invalidate all ring entry labels — LVGL objects are gone after lv_obj_clean
  for (int i = 0; i < PM_RING_SIZE; i++) g_pm_ring[i].status_label = nullptr;
  // Invalidate pending translation bubble pointers
  translate_invalidate_bubbles();
  scroll_btn_ensure();
}
