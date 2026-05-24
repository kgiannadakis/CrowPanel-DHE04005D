// ui_homescreen.c — Dark messenger homescreen (chat list + chat view)
// 480 × 800 portrait, ESP32-S3, LVGL 8.3

#include "ui.h"
#include "ui_tabbar.h"

// ── Widget pointers ─────────────────────────────────────────

lv_obj_t * ui_homescreen       = NULL;
lv_obj_t * ui_devicenamepanel  = NULL;
lv_obj_t * ui_devicenamelabel  = NULL;
lv_obj_t * ui_chatpanel        = NULL;
lv_obj_t * ui_mutebutton       = NULL;
lv_obj_t * ui_mutelabel        = NULL;
lv_obj_t * ui_searchfield      = NULL;
lv_obj_t * ui_textsendtype     = NULL;
lv_obj_t * ui_backbutton       = NULL;
lv_obj_t * ui_backlabel        = NULL;
lv_obj_t * ui_timelabel        = NULL;
lv_obj_t * ui_Keyboard1        = NULL;

// Stubs — SquareLine-era symbols still referenced in display.cpp widget arrays.
lv_obj_t * ui_homebutton1      = NULL;
lv_obj_t * ui_homeabel1        = NULL;
lv_obj_t * ui_settingsbutton1  = NULL;
lv_obj_t * ui_settingslabel1   = NULL;

// ── Helpers ─────────────────────────────────────────────────

static void style_textarea(lv_obj_t * ta) {
    lv_obj_set_style_bg_color(ta, lv_color_hex(TH_INPUT), 0);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ta, 1, 0);
    lv_obj_set_style_border_color(ta, lv_color_hex(TH_BORDER), 0);
    lv_obj_set_style_border_opa(ta, LV_OPA_80, 0);
    lv_obj_set_style_radius(ta, 22, 0);
    lv_obj_set_style_text_color(ta, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_16, 0);
    lv_obj_set_style_pad_left(ta, 16, 0);
    lv_obj_set_style_pad_right(ta, 16, 0);
    lv_obj_set_style_pad_top(ta, 8, 0);
    lv_obj_set_style_pad_bottom(ta, 8, 0);
    lv_obj_set_style_text_color(ta, lv_color_hex(TH_TEXT3),
                                 LV_PART_TEXTAREA_PLACEHOLDER);
}

// ── Screen init ─────────────────────────────────────────────

void ui_homescreen_screen_init(void) {
    ui_homescreen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(ui_homescreen, lv_color_hex(TH_BG), 0);
    lv_obj_set_style_bg_opa(ui_homescreen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(ui_homescreen, LV_OBJ_FLAG_SCROLLABLE);

    // ── STATUS BAR (0–50) ───────────────────────────────────

    lv_obj_t * statusbar = lv_obj_create(ui_homescreen);
    lv_obj_set_size(statusbar, SCR_W, STATUS_H);
    lv_obj_set_pos(statusbar, 0, 0);
    lv_obj_set_style_bg_color(statusbar, lv_color_hex(TH_SURFACE), 0);
    lv_obj_set_style_bg_opa(statusbar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(statusbar, 1, 0);
    lv_obj_set_style_border_color(statusbar, lv_color_hex(TH_SEPARATOR), 0);
    lv_obj_set_style_border_side(statusbar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_radius(statusbar, 0, 0);
    lv_obj_set_style_pad_left(statusbar, 14, 0);
    lv_obj_set_style_pad_right(statusbar, 14, 0);
    lv_obj_clear_flag(statusbar, LV_OBJ_FLAG_SCROLLABLE);

    // [1] Device name — CENTERED
    ui_devicenamepanel = statusbar;
    ui_devicenamelabel = lv_label_create(statusbar);
    lv_label_set_text(ui_devicenamelabel, "MeshCore");
    lv_label_set_long_mode(ui_devicenamelabel, LV_LABEL_LONG_DOT);
    lv_obj_set_width(ui_devicenamelabel, 300);
    lv_obj_set_style_text_align(ui_devicenamelabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(ui_devicenamelabel, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(ui_devicenamelabel, &lv_font_montserrat_20, 0);
    lv_obj_center(ui_devicenamelabel);

    // Time label (right side)
    ui_timelabel = lv_label_create(statusbar);
    lv_label_set_text(ui_timelabel, "--:--");
    lv_obj_set_style_text_color(ui_timelabel, lv_color_hex(TH_ACCENT_LIGHT), 0);
    lv_obj_set_style_text_font(ui_timelabel, &lv_font_montserrat_18, 0);
    lv_obj_align(ui_timelabel, LV_ALIGN_RIGHT_MID, 0, 0);

    // [3] Back button — HIDDEN by default. main.cpp shows it in enter_chat_mode.
    ui_backbutton = lv_btn_create(ui_homescreen);
    lv_obj_set_size(ui_backbutton, 50, STATUS_H);
    lv_obj_set_pos(ui_backbutton, 0, 0);
    lv_obj_set_style_bg_opa(ui_backbutton, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(ui_backbutton, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_opa(ui_backbutton, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(ui_backbutton, 0, 0);
    ui_backlabel = lv_label_create(ui_backbutton);
    lv_label_set_text(ui_backlabel, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(ui_backlabel, lv_color_hex(TH_ACCENT), 0);
    lv_obj_set_style_text_font(ui_backlabel, &lv_font_montserrat_22, 0);
    lv_obj_center(ui_backlabel);
    lv_obj_add_flag(ui_backbutton, LV_OBJ_FLAG_HIDDEN);

    // Mute button — hidden initially (smaller in landscape)
    ui_mutebutton = lv_btn_create(ui_homescreen);
    {
      int16_t is_land = (SCR_H < SCR_W);
      lv_obj_set_size(ui_mutebutton, is_land ? 38 : 50, is_land ? 28 : 42);
      lv_obj_set_pos(ui_mutebutton, SCR_W - 130, is_land ? 6 : 4);
    }
    lv_obj_set_style_radius(ui_mutebutton, 12, 0);
    lv_obj_set_style_bg_color(ui_mutebutton, lv_color_hex(TH_GREEN), 0);
    lv_obj_set_style_bg_opa(ui_mutebutton, LV_OPA_COVER, 0);
    lv_obj_set_style_border_opa(ui_mutebutton, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_opa(ui_mutebutton, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(ui_mutebutton, 0, 0);
    ui_mutelabel = lv_label_create(ui_mutebutton);
    lv_label_set_text(ui_mutelabel, LV_SYMBOL_VOLUME_MAX);
    lv_obj_set_style_text_color(ui_mutelabel, lv_color_white(), 0);
    lv_obj_set_style_text_font(ui_mutelabel,
        (SCR_H < SCR_W) ? &lv_font_montserrat_14 : &lv_font_montserrat_18, 0);
    lv_obj_center(ui_mutelabel);
    lv_obj_add_flag(ui_mutebutton, LV_OBJ_FLAG_HIDDEN);

    // ── SEARCH FIELD ────────────────────────────────────────
    // Center-aligned, y=-324 → screen y≈76, right below status bar.

    ui_searchfield = lv_textarea_create(ui_homescreen);
    lv_obj_set_align(ui_searchfield, LV_ALIGN_CENTER);
    lv_obj_set_size(ui_searchfield, SCR_W - 24, 44);
    lv_obj_set_pos(ui_searchfield, 0, SEARCH_Y_OFFSET);
    lv_textarea_set_one_line(ui_searchfield, true);
    lv_textarea_set_placeholder_text(ui_searchfield, LV_SYMBOL_EYE_OPEN " Search...");
    style_textarea(ui_searchfield);

    // ── CHAT PANEL ──────────────────────────────────────────
    // [2] Starts at y=100 so bubbles sit below the search field.

    ui_chatpanel = lv_obj_create(ui_homescreen);
    lv_obj_set_pos(ui_chatpanel, 0, CHATPANEL_START_Y);
    lv_obj_set_size(ui_chatpanel, SCR_W, SCR_H - CHATPANEL_START_Y);
    lv_obj_set_style_bg_color(ui_chatpanel, lv_color_hex(TH_BG), 0);
    lv_obj_set_style_bg_opa(ui_chatpanel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_opa(ui_chatpanel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(ui_chatpanel, 0, 0);
    lv_obj_set_style_pad_all(ui_chatpanel, 6, 0);

    // ── TEXT INPUT ──────────────────────────────────────────
    // [4] y=305 → screen y=705, bottom at 727, above tab bar (740).

    ui_textsendtype = lv_textarea_create(ui_homescreen);
    lv_obj_set_align(ui_textsendtype, LV_ALIGN_TOP_MID);
    lv_obj_set_size(ui_textsendtype, SCR_W - 16, 44);
    lv_obj_set_pos(ui_textsendtype, 0, TEXTSEND_Y_DEFAULT);
    lv_textarea_set_one_line(ui_textsendtype, true);
    lv_textarea_set_placeholder_text(ui_textsendtype, "Type a message...");
    style_textarea(ui_textsendtype);
    lv_obj_add_flag(ui_textsendtype, LV_OBJ_FLAG_HIDDEN);

    // ── TAB BAR ─────────────────────────────────────────────

    ui_tabbar_create(ui_homescreen, 0);

    // ── KEYBOARD ────────────────────────────────────────────

    ui_Keyboard1 = lv_keyboard_create(ui_homescreen);
    lv_obj_set_align(ui_Keyboard1, LV_ALIGN_TOP_LEFT);
    lv_obj_set_size(ui_Keyboard1, SCR_W, KB_HEIGHT);
    lv_obj_set_pos(ui_Keyboard1, 0, KB_Y_OFFSET);
    lv_obj_add_flag(ui_Keyboard1, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(ui_Keyboard1, lv_color_hex(TH_SURFACE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui_Keyboard1, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui_Keyboard1, lv_color_hex(TH_SURFACE2),
                               LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui_Keyboard1, lv_color_hex(TH_TEXT),
                                 LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_Keyboard1, lv_color_hex(TH_ACCENT),
                               LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(ui_Keyboard1, lv_color_white(),
                                 LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_border_opa(ui_Keyboard1, LV_OPA_TRANSP,
                                 LV_PART_ITEMS | LV_STATE_DEFAULT);
}

void ui_homescreen_screen_destroy(void) {
    if (ui_homescreen) { lv_obj_del(ui_homescreen); ui_homescreen = NULL; }
}
