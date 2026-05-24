// features_ui.c — Features & Bridges settings screen
// 480 x 800 portrait, ESP32-S3, LVGL 8.3

#include <stdio.h>
#include "ui.h"
#include "features_ui.h"
#include "ui_theme.h"
#include "ui_tabbar.h"

// FIRMWARE_VERSION is defined in platformio.ini build flags

// ── Widget pointers ─────────────────────────────────────────

lv_obj_t* ui_featuresscreen = NULL;

// Translation
lv_obj_t* ui_autotranslate_toggle = NULL;
lv_obj_t* ui_autotranslate_lbl    = NULL;
lv_obj_t* ui_translate_lang_dd    = NULL;

// Telegram
lv_obj_t* ui_tg_toggle     = NULL;
lv_obj_t* ui_tg_toggle_lbl = NULL;
lv_obj_t* ui_tg_token_ta   = NULL;
lv_obj_t* ui_tg_chatid_ta  = NULL;
lv_obj_t* ui_tg_status_lbl = NULL;

// Web dashboard
lv_obj_t* ui_wd_toggle     = NULL;
lv_obj_t* ui_wd_toggle_lbl = NULL;
lv_obj_t* ui_wd_status_lbl = NULL;

// OTA
lv_obj_t* ui_ota_repo_ta      = NULL;
lv_obj_t* ui_ota_check_btn    = NULL;
lv_obj_t* ui_ota_check_lbl    = NULL;
lv_obj_t* ui_ota_status_lbl   = NULL;
lv_obj_t* ui_ota_progress_bar = NULL;

// WiFi
lv_obj_t* ui_wifitoggle          = NULL;
lv_obj_t* ui_wifitoggle_lbl      = NULL;
lv_obj_t* ui_wifiscanbutton      = NULL;
lv_obj_t* ui_wifiscan_lbl        = NULL;
lv_obj_t* ui_wifinetworksdropdown = NULL;
lv_obj_t* ui_wifipassword        = NULL;
lv_obj_t* ui_wificonnectbutton   = NULL;
lv_obj_t* ui_wificonnect_lbl     = NULL;
lv_obj_t* ui_wififorgetbutton    = NULL;
lv_obj_t* ui_wififorget_lbl      = NULL;
lv_obj_t* ui_wifistatuslabel     = NULL;

// Keyboard
lv_obj_t* ui_FeaturesKB = NULL;

// ── Helpers (same as ui_settingscreen.c) ────────────────────

static lv_obj_t* make_section_hdr(lv_obj_t* parent, const char* txt) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, txt);
    lv_obj_set_width(lbl, lv_pct(100));
    lv_obj_set_style_text_color(lbl, lv_color_hex(TH_ACCENT_LIGHT), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_pad_top(lbl, 16, 0);
    lv_obj_set_style_pad_bottom(lbl, 6, 0);
    return lbl;
}

static lv_obj_t* make_action_btn(lv_obj_t* parent, lv_obj_t** lbl_out,
                                  lv_coord_t w, lv_coord_t h) {
    lv_obj_t* btn = lv_btn_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_radius(btn, 14, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(TH_ACCENT), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(btn, 4, 0);
    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, "");
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(lbl);
    if (lbl_out) *lbl_out = lbl;
    return btn;
}

static void style_ta(lv_obj_t* ta) {
    lv_obj_set_style_bg_color(ta, lv_color_hex(TH_INPUT), 0);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ta, 1, 0);
    lv_obj_set_style_border_color(ta, lv_color_hex(TH_BORDER), 0);
    lv_obj_set_style_border_opa(ta, LV_OPA_80, 0);
    lv_obj_set_style_radius(ta, 12, 0);
    lv_obj_set_style_text_color(ta, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_16, 0);
    lv_obj_set_style_pad_left(ta, 12, 0);
    lv_obj_set_style_pad_right(ta, 12, 0);
    lv_obj_set_style_pad_top(ta, 6, 0);
    lv_obj_set_style_pad_bottom(ta, 6, 0);
    lv_obj_set_style_text_color(ta, lv_color_hex(TH_TEXT3),
                                 LV_PART_TEXTAREA_PLACEHOLDER);
}

static void style_dd(lv_obj_t* dd) {
    lv_obj_set_style_bg_color(dd, lv_color_hex(TH_INPUT), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dd, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(dd, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(dd, lv_color_hex(TH_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_opa(dd, LV_OPA_80, LV_PART_MAIN);
    lv_obj_set_style_radius(dd, 12, LV_PART_MAIN);
    lv_obj_set_style_text_color(dd, lv_color_hex(TH_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(dd, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_pad_left(dd, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_right(dd, 12, LV_PART_MAIN);
    lv_obj_set_height(dd, 44);
}

// ── Screen init ─────────────────────────────────────────────

void ui_featuresscreen_screen_init(void) {
    ui_featuresscreen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(ui_featuresscreen, lv_color_hex(TH_BG), 0);
    lv_obj_set_style_bg_opa(ui_featuresscreen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(ui_featuresscreen, LV_OBJ_FLAG_SCROLLABLE);

    // ── HEADER BAR ──────────────────────────────────────────

    lv_obj_t* hdr = lv_obj_create(ui_featuresscreen);
    lv_obj_set_size(hdr, SCR_W, STATUS_H);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_set_style_bg_color(hdr, lv_color_hex(TH_SURFACE), 0);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(hdr, 1, 0);
    lv_obj_set_style_border_color(hdr, lv_color_hex(TH_SEPARATOR), 0);
    lv_obj_set_style_border_side(hdr, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_radius(hdr, 0, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(hdr);
    lv_label_set_text(title, LV_SYMBOL_WIFI "  Web Apps");
    lv_obj_set_style_text_color(title, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_center(title);

    // ── SCROLLABLE FORM ─────────────────────────────────────

    lv_obj_t* form = lv_obj_create(ui_featuresscreen);
    lv_obj_set_pos(form, 0, STATUS_H);
    lv_obj_set_size(form, SCR_W, SCR_H - STATUS_H - TAB_H);
    lv_obj_set_style_bg_opa(form, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(form, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(form, 0, 0);
    lv_obj_set_style_pad_left(form, 14, 0);
    lv_obj_set_style_pad_right(form, 14, 0);
    lv_obj_set_style_pad_top(form, 4, 0);
    lv_obj_set_style_pad_bottom(form, 40, 0);
    lv_obj_set_style_pad_row(form, 6, 0);
    lv_obj_set_flex_flow(form, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(form, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_clear_flag(form, LV_OBJ_FLAG_SCROLL_ELASTIC);

    // ══════════════════════════════════════════════════════════
    // WIFI SETTINGS
    // ══════════════════════════════════════════════════════════

    make_section_hdr(form, "WIFI SETTINGS");
    {
        lv_obj_t* hint = lv_label_create(form);
        lv_label_set_text(hint, "Connect once to sync clock");
        lv_obj_set_width(hint, lv_pct(100));
        lv_obj_set_style_text_color(hint, lv_color_hex(TH_TEXT3), 0);
        lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
        lv_obj_set_style_pad_bottom(hint, 2, 0);
    }

    ui_wifitoggle = make_action_btn(form, &ui_wifitoggle_lbl, lv_pct(100), 72);

    {
        lv_obj_t* wifi_btn_row = lv_obj_create(form);
        lv_obj_set_size(wifi_btn_row, lv_pct(100), 54);
        lv_obj_set_style_bg_opa(wifi_btn_row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_opa(wifi_btn_row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_all(wifi_btn_row, 0, 0);
        lv_obj_set_style_pad_column(wifi_btn_row, 10, 0);
        lv_obj_set_flex_flow(wifi_btn_row, LV_FLEX_FLOW_ROW);
        lv_obj_clear_flag(wifi_btn_row, LV_OBJ_FLAG_SCROLLABLE);

        ui_wifiscanbutton = make_action_btn(wifi_btn_row, &ui_wifiscan_lbl, BTN_HALF_W, 50);
        ui_wififorgetbutton = make_action_btn(wifi_btn_row, &ui_wififorget_lbl, BTN_HALF_W, 50);
        lv_obj_set_style_bg_color(ui_wififorgetbutton, lv_color_hex(TH_RED), 0);
        lv_obj_add_flag(ui_wififorgetbutton, LV_OBJ_FLAG_HIDDEN);
    }

    ui_wifinetworksdropdown = lv_dropdown_create(form);
    lv_obj_set_width(ui_wifinetworksdropdown, lv_pct(100));
    lv_dropdown_set_options(ui_wifinetworksdropdown, "(no networks found)");
    style_dd(ui_wifinetworksdropdown);
    lv_obj_add_flag(ui_wifinetworksdropdown, LV_OBJ_FLAG_HIDDEN);

    ui_wifipassword = lv_textarea_create(form);
    lv_obj_set_size(ui_wifipassword, lv_pct(100), 44);
    lv_textarea_set_one_line(ui_wifipassword, true);
    lv_textarea_set_placeholder_text(ui_wifipassword, "WiFi password...");
    lv_textarea_set_password_mode(ui_wifipassword, true);
    style_ta(ui_wifipassword);
    lv_obj_add_flag(ui_wifipassword, LV_OBJ_FLAG_HIDDEN);

    ui_wificonnectbutton = make_action_btn(form, &ui_wificonnect_lbl, lv_pct(100), 48);
    lv_obj_add_flag(ui_wificonnectbutton, LV_OBJ_FLAG_HIDDEN);

    ui_wifistatuslabel = lv_label_create(form);
    lv_label_set_text(ui_wifistatuslabel, "");
    lv_obj_set_width(ui_wifistatuslabel, lv_pct(100));
    lv_label_set_long_mode(ui_wifistatuslabel, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(ui_wifistatuslabel, lv_color_hex(TH_TEXT2), 0);
    lv_obj_set_style_text_font(ui_wifistatuslabel, &lv_font_montserrat_14, 0);

    // ══════════════════════════════════════════════════════════
    // TRANSLATION
    // ══════════════════════════════════════════════════════════

    make_section_hdr(form, "TRANSLATION (available only when online)");
    {
        lv_obj_t* hint = lv_label_create(form);
        lv_label_set_text(hint, "Auto-translate or long-press a message");
        lv_obj_set_width(hint, lv_pct(100));
        lv_obj_set_style_text_color(hint, lv_color_hex(TH_TEXT3), 0);
        lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
        lv_obj_set_style_pad_bottom(hint, 2, 0);
    }

    ui_autotranslate_toggle = make_action_btn(form, &ui_autotranslate_lbl, lv_pct(100), 48);

    ui_translate_lang_dd = lv_dropdown_create(form);
    lv_obj_set_width(ui_translate_lang_dd, lv_pct(100));
    style_dd(ui_translate_lang_dd);

    // ══════════════════════════════════════════════════════════
    // WEB DASHBOARD
    // ══════════════════════════════════════════════════════════

    // ══════════════════════════════════════════════════════════
    // TELEGRAM BRIDGE
    // ══════════════════════════════════════════════════════════

    make_section_hdr(form, "TELEGRAM BRIDGE");

    ui_tg_toggle = make_action_btn(form, &ui_tg_toggle_lbl, lv_pct(100), 44);

    lv_obj_t* tok_lbl = lv_label_create(form);
    lv_label_set_text(tok_lbl, "Bot Token");
    lv_obj_set_style_text_color(tok_lbl, lv_color_hex(TH_TEXT2), 0);
    lv_obj_set_style_text_font(tok_lbl, &lv_font_montserrat_14, 0);

    ui_tg_token_ta = lv_textarea_create(form);
    lv_textarea_set_one_line(ui_tg_token_ta, true);
    lv_textarea_set_placeholder_text(ui_tg_token_ta, "Paste bot token from @BotFather");
    lv_textarea_set_password_show_time(ui_tg_token_ta, 0);
    lv_textarea_set_password_mode(ui_tg_token_ta, true);
    lv_obj_set_width(ui_tg_token_ta, lv_pct(100));
    style_ta(ui_tg_token_ta);

    lv_obj_t* cid_lbl = lv_label_create(form);
    lv_label_set_text(cid_lbl, "Chat ID");
    lv_obj_set_style_text_color(cid_lbl, lv_color_hex(TH_TEXT2), 0);
    lv_obj_set_style_text_font(cid_lbl, &lv_font_montserrat_14, 0);

    ui_tg_chatid_ta = lv_textarea_create(form);
    lv_textarea_set_one_line(ui_tg_chatid_ta, true);
    lv_textarea_set_placeholder_text(ui_tg_chatid_ta, "e.g. -1001234567890");
    lv_obj_set_width(ui_tg_chatid_ta, lv_pct(100));
    style_ta(ui_tg_chatid_ta);

    ui_tg_status_lbl = lv_label_create(form);
    lv_label_set_text(ui_tg_status_lbl, "Disabled");
    lv_obj_set_style_text_color(ui_tg_status_lbl, lv_color_hex(TH_TEXT3), 0);
    lv_obj_set_style_text_font(ui_tg_status_lbl, &lv_font_montserrat_14, 0);

    // ── TAB BAR ─────────────────────────────────────────────

    ui_tabbar_create(ui_featuresscreen, 2);

    // ── KEYBOARD ────────────────────────────────────────────

    ui_FeaturesKB = lv_keyboard_create(ui_featuresscreen);
    lv_obj_set_align(ui_FeaturesKB, LV_ALIGN_TOP_LEFT);
    lv_obj_set_size(ui_FeaturesKB, SCR_W, KB_HEIGHT);
    lv_obj_set_pos(ui_FeaturesKB, 0, KB_Y_OFFSET);
    lv_obj_add_flag(ui_FeaturesKB, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(ui_FeaturesKB, lv_color_hex(TH_SURFACE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui_FeaturesKB, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui_FeaturesKB, lv_color_hex(TH_SURFACE2),
                               LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui_FeaturesKB, lv_color_hex(TH_TEXT),
                                 LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_FeaturesKB, lv_color_hex(TH_ACCENT),
                               LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(ui_FeaturesKB, lv_color_white(),
                                 LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_border_opa(ui_FeaturesKB, LV_OPA_TRANSP,
                                 LV_PART_ITEMS | LV_STATE_DEFAULT);
}

void ui_featuresscreen_screen_destroy(void) {
    if (ui_featuresscreen) { lv_obj_del(ui_featuresscreen); ui_featuresscreen = NULL; }
}
