// ui_settingscreen.c — Dark settings panel (scrollable form)
// 480 × 800 portrait, ESP32-S3, LVGL 8.3

#include "ui.h"
#include "ui_tabbar.h"

// ── Widget pointers ─────────────────────────────────────────

lv_obj_t * ui_settingscreen       = NULL;
lv_obj_t * ui_presetpickbutton    = NULL;   // NULL — removed (auto-apply on select)
lv_obj_t * ui_Label5              = NULL;
lv_obj_t * ui_brightnessslider    = NULL;
lv_obj_t * ui_zeroadvertbutton    = NULL;
lv_obj_t * ui_Label8              = NULL;
lv_obj_t * ui_fadvertbutton       = NULL;
lv_obj_t * ui_Label9              = NULL;
lv_obj_t * ui_positionadverttoggle = NULL;
lv_obj_t * ui_positionadvert_lbl   = NULL;
lv_obj_t * ui_Label10             = NULL;
lv_obj_t * ui_autocontacttoggle   = NULL;
lv_obj_t * ui_Label11             = NULL;
lv_obj_t * ui_presetsdropdown     = NULL;
lv_obj_t * ui_serialmonitorwindow = NULL;
lv_obj_t * ui_Label15             = NULL;
lv_obj_t * ui_screentimeout       = NULL;
lv_obj_t * ui_Label1              = NULL;
lv_obj_t * ui_hashtagchannel      = NULL;
lv_obj_t * ui_notificationstoggle = NULL;
lv_obj_t * ui_Label20             = NULL;
lv_obj_t * ui_autorepeatertoggle  = NULL;
lv_obj_t * ui_Label18             = NULL;
lv_obj_t * ui_repeatersbutton     = NULL;   // NULL — removed (tab handles it)
lv_obj_t * ui_Label2              = NULL;
lv_obj_t * ui_purgedatabutton     = NULL;
lv_obj_t * ui_Label21             = NULL;
lv_obj_t * ui_renamebox           = NULL;
lv_obj_t * ui_timezonedropdown    = NULL;
lv_obj_t * ui_txpowerslider       = NULL;
lv_obj_t * ui_txpowerlabel        = NULL;
lv_obj_t * ui_Keyboard2           = NULL;


// Orientation toggle
lv_obj_t * ui_orientationtoggle = NULL;
lv_obj_t * ui_orientation_lbl   = NULL;
lv_obj_t * ui_settings_ser_btncol = NULL;


// Stubs — SquareLine-era symbols still referenced in display.cpp widget arrays.
lv_obj_t * ui_homebutton3     = NULL;
lv_obj_t * ui_homeabel3       = NULL;
lv_obj_t * ui_settingsbutton3 = NULL;
lv_obj_t * ui_settingslabel3  = NULL;
lv_obj_t * ui_backbutton2     = NULL;
lv_obj_t * ui_backlabel2      = NULL;
lv_obj_t * ui_Button3         = NULL;

// ── Helpers ─────────────────────────────────────────────────

static lv_obj_t * make_section_hdr(lv_obj_t * parent, const char * txt) {
    lv_obj_t * lbl = lv_label_create(parent);
    lv_label_set_text(lbl, txt);
    lv_obj_set_width(lbl, lv_pct(100));
    lv_obj_set_style_text_color(lbl, lv_color_hex(TH_ACCENT_LIGHT), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_pad_top(lbl, 16, 0);
    lv_obj_set_style_pad_bottom(lbl, 6, 0);
    return lbl;
}

static lv_obj_t * make_action_btn(lv_obj_t * parent, lv_obj_t ** lbl_out,
                                   lv_coord_t w, lv_coord_t h) {
    lv_obj_t * btn = lv_btn_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_radius(btn, 14, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(TH_ACCENT), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(btn, 4, 0);
    lv_obj_t * lbl = lv_label_create(btn);
    lv_label_set_text(lbl, "");
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(lbl);
    if (lbl_out) *lbl_out = lbl;
    return btn;
}

static lv_obj_t * make_toggle_row(lv_obj_t * parent, lv_obj_t ** lbl_out, const char * txt) {
    lv_obj_t * row = lv_obj_create(parent);
    lv_obj_set_size(row, lv_pct(100), 52);
    lv_obj_set_style_radius(row, 14, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(TH_SURFACE2), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(TH_BORDER), 0);
    lv_obj_set_style_shadow_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_hor(row, 14, 0);
    lv_obj_set_style_pad_ver(row, 10, 0);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * lbl = lv_label_create(row);
    lv_label_set_text(lbl, txt);
    lv_obj_set_flex_grow(lbl, 1);
    lv_obj_set_style_text_color(lbl, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);

    lv_obj_t * sw = lv_switch_create(row);
    lv_obj_set_size(sw, 54, 30);
    lv_obj_set_style_bg_color(sw, lv_color_hex(TH_BORDER), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, lv_color_hex(TH_ACCENT), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_KNOB);

    if (lbl_out) *lbl_out = lbl;
    return sw;
}

static void style_ta(lv_obj_t * ta) {
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

static void style_dd(lv_obj_t * dd) {
    lv_obj_set_style_bg_color(dd, lv_color_hex(TH_INPUT), 0);
    lv_obj_set_style_bg_opa(dd, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dd, 1, 0);
    lv_obj_set_style_border_color(dd, lv_color_hex(TH_BORDER), 0);
    lv_obj_set_style_radius(dd, 12, 0);
    lv_obj_set_style_text_color(dd, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(dd, &lv_font_montserrat_16, 0);
    lv_obj_set_style_pad_all(dd, 8, 0);
}

// ── Screen init ─────────────────────────────────────────────

void ui_settingscreen_screen_init(void) {
    ui_settingscreen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(ui_settingscreen, lv_color_hex(TH_BG), 0);
    lv_obj_set_style_bg_opa(ui_settingscreen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(ui_settingscreen, LV_OBJ_FLAG_SCROLLABLE);

    // ── HEADER BAR ──────────────────────────────────────────

    lv_obj_t * hdr = lv_obj_create(ui_settingscreen);
    lv_obj_set_size(hdr, SCR_W, STATUS_H);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_set_style_bg_color(hdr, lv_color_hex(TH_SURFACE), 0);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(hdr, 1, 0);
    lv_obj_set_style_border_color(hdr, lv_color_hex(TH_SEPARATOR), 0);
    lv_obj_set_style_border_side(hdr, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_radius(hdr, 0, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * title = lv_label_create(hdr);
    lv_label_set_text(title, LV_SYMBOL_SETTINGS "  Settings");
    lv_obj_set_style_text_color(title, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_center(title);

    // ── SCROLLABLE FORM ─────────────────────────────────────

    lv_obj_t * form = lv_obj_create(ui_settingscreen);
    lv_obj_set_pos(form, 0, STATUS_H);
    lv_obj_set_size(form, SCR_W, SCR_H - STATUS_H - TAB_H);
    lv_obj_set_style_bg_opa(form, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(form, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(form, 0, 0);
    lv_obj_set_style_pad_left(form, 14, 0);
    lv_obj_set_style_pad_right(form, 14, 0);
    lv_obj_set_style_pad_top(form, 4, 0);
    lv_obj_set_style_pad_bottom(form, 20, 0);
    lv_obj_set_style_pad_row(form, 6, 0);
    lv_obj_set_flex_flow(form, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(form, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_clear_flag(form, LV_OBJ_FLAG_SCROLL_ELASTIC);

    // ══════════════════════════════════════════════════════════
    // [7] SERIAL MONITOR — at the top for quick visibility
    //     Landscape: monitor 400px + button column side by side
    // ══════════════════════════════════════════════════════════

    make_section_hdr(form, "SERIAL MONITOR");

    ui_settings_ser_btncol = NULL;

    if (SCR_H < SCR_W) {
      // ── Landscape: row with monitor (400px) + button column ──
      lv_obj_t * ser_row = lv_obj_create(form);
      lv_obj_set_size(ser_row, lv_pct(100), LV_SIZE_CONTENT);
      lv_obj_set_style_min_height(ser_row, 220, 0);
      lv_obj_set_style_bg_opa(ser_row, LV_OPA_TRANSP, 0);
      lv_obj_set_style_border_opa(ser_row, LV_OPA_TRANSP, 0);
      lv_obj_set_style_pad_all(ser_row, 0, 0);
      lv_obj_set_style_pad_column(ser_row, 8, 0);
      lv_obj_set_flex_flow(ser_row, LV_FLEX_FLOW_ROW);
      lv_obj_clear_flag(ser_row, LV_OBJ_FLAG_SCROLLABLE);

      ui_serialmonitorwindow = lv_obj_create(ser_row);
      lv_obj_set_size(ui_serialmonitorwindow, 400, 220);

      // Button column to the right (flex-grow fills remaining ~370px)
      ui_settings_ser_btncol = lv_obj_create(ser_row);
      lv_obj_set_height(ui_settings_ser_btncol, LV_SIZE_CONTENT);
      lv_obj_set_flex_grow(ui_settings_ser_btncol, 1);
      lv_obj_set_style_bg_opa(ui_settings_ser_btncol, LV_OPA_TRANSP, 0);
      lv_obj_set_style_border_opa(ui_settings_ser_btncol, LV_OPA_TRANSP, 0);
      lv_obj_set_style_pad_all(ui_settings_ser_btncol, 0, 0);
      lv_obj_set_style_pad_row(ui_settings_ser_btncol, 6, 0);
      lv_obj_set_flex_flow(ui_settings_ser_btncol, LV_FLEX_FLOW_COLUMN);
      lv_obj_clear_flag(ui_settings_ser_btncol, LV_OBJ_FLAG_SCROLLABLE);

      ui_autocontacttoggle  = make_action_btn(ui_settings_ser_btncol, &ui_Label11, lv_pct(96), 46);
      ui_autorepeatertoggle = make_action_btn(ui_settings_ser_btncol, &ui_Label18, lv_pct(96), 46);
      // g_discover_repeaters_btn + Floor Noise added by display.cpp into this column
    } else {
      // ── Portrait: full-width monitor, buttons in a row below ──
      ui_serialmonitorwindow = lv_obj_create(form);
      lv_obj_set_size(ui_serialmonitorwindow, lv_pct(100), 220);

      {
        lv_obj_t * auto_row = lv_obj_create(form);
        lv_obj_set_size(auto_row, lv_pct(100), 54);
        lv_obj_set_style_bg_opa(auto_row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_opa(auto_row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_all(auto_row, 0, 0);
        lv_obj_set_style_pad_column(auto_row, 10, 0);
        lv_obj_set_flex_flow(auto_row, LV_FLEX_FLOW_ROW);
        lv_obj_clear_flag(auto_row, LV_OBJ_FLAG_SCROLLABLE);

        ui_autocontacttoggle  = make_action_btn(auto_row, &ui_Label11, BTN_HALF_W, 50);
        ui_autorepeatertoggle = make_action_btn(auto_row, &ui_Label18, BTN_HALF_W, 50);
      }
      // g_discover_repeaters_btn will be created by display.cpp as next flex sibling
    }

    // Common serial monitor styling
    lv_obj_set_style_bg_color(ui_serialmonitorwindow, lv_color_hex(0x0A0F14), 0);
    lv_obj_set_style_bg_opa(ui_serialmonitorwindow, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ui_serialmonitorwindow, 1, 0);
    lv_obj_set_style_border_color(ui_serialmonitorwindow, lv_color_hex(TH_BORDER), 0);
    lv_obj_set_style_radius(ui_serialmonitorwindow, 8, 0);
    lv_obj_set_style_pad_all(ui_serialmonitorwindow, 8, 0);
    lv_obj_set_scroll_dir(ui_serialmonitorwindow, LV_DIR_VER);

    // ══════════════════════════════════════════════════════════
    // DEVICE NAME
    // ══════════════════════════════════════════════════════════

    make_section_hdr(form, "DEVICE NAME");

    ui_renamebox = lv_textarea_create(form);
    lv_obj_set_size(ui_renamebox, lv_pct(100), 44);
    lv_textarea_set_one_line(ui_renamebox, true);
    lv_textarea_set_placeholder_text(ui_renamebox, "Enter device name...");
    style_ta(ui_renamebox);

    // ══════════════════════════════════════════════════════════
    // JOIN CHANNEL
    // ══════════════════════════════════════════════════════════

    make_section_hdr(form, "JOIN CHANNEL");

    ui_hashtagchannel = lv_textarea_create(form);
    lv_obj_set_size(ui_hashtagchannel, lv_pct(100), 44);
    lv_textarea_set_one_line(ui_hashtagchannel, true);
    lv_textarea_set_placeholder_text(ui_hashtagchannel, "#channel or name|key");
    style_ta(ui_hashtagchannel);

    // ══════════════════════════════════════════════════════════
    // [8] RADIO PRESET — dropdown auto-applies, shows current
    // ══════════════════════════════════════════════════════════

    make_section_hdr(form, "RADIO PRESET");

    ui_presetsdropdown = lv_dropdown_create(form);
    lv_obj_set_width(ui_presetsdropdown, lv_pct(100));
    // NO lv_dropdown_set_text() — shows the actual selected option name
    style_dd(ui_presetsdropdown);

    // ══════════════════════════════════════════════════════════
    // TX POWER & SCREEN TIMEOUT — side by side
    // ══════════════════════════════════════════════════════════

    {
      lv_obj_t * pw_row = lv_obj_create(form);
      lv_obj_set_size(pw_row, lv_pct(100), LV_SIZE_CONTENT);
      lv_obj_set_style_bg_opa(pw_row, LV_OPA_TRANSP, 0);
      lv_obj_set_style_border_opa(pw_row, LV_OPA_TRANSP, 0);
      lv_obj_set_style_pad_all(pw_row, 0, 0);
      lv_obj_set_style_pad_column(pw_row, 14, 0);
      lv_obj_set_style_pad_row(pw_row, 4, 0);
      lv_obj_set_flex_flow(pw_row, LV_FLEX_FLOW_ROW);
      lv_obj_clear_flag(pw_row, LV_OBJ_FLAG_SCROLLABLE);

      // Left: TX POWER
      lv_obj_t * tx_col = lv_obj_create(pw_row);
      lv_obj_set_size(tx_col, lv_pct(46), LV_SIZE_CONTENT);
      lv_obj_set_style_bg_opa(tx_col, LV_OPA_TRANSP, 0);
      lv_obj_set_style_border_opa(tx_col, LV_OPA_TRANSP, 0);
      lv_obj_set_style_pad_all(tx_col, 0, 0);
      lv_obj_set_style_pad_row(tx_col, 4, 0);
      lv_obj_set_flex_flow(tx_col, LV_FLEX_FLOW_COLUMN);
      lv_obj_clear_flag(tx_col, LV_OBJ_FLAG_SCROLLABLE);

      lv_obj_t * tx_lbl = lv_label_create(tx_col);
      lv_label_set_text(tx_lbl, "TX POWER (1-22 dBm)");
      lv_obj_set_style_text_color(tx_lbl, lv_color_hex(TH_ACCENT_LIGHT), 0);
      lv_obj_set_style_text_font(tx_lbl, &lv_font_montserrat_14, 0);

      ui_txpowerslider = lv_textarea_create(tx_col);
      lv_obj_set_size(ui_txpowerslider, lv_pct(100), 44);
      lv_textarea_set_one_line(ui_txpowerslider, true);
      lv_textarea_set_accepted_chars(ui_txpowerslider, "0123456789");
      lv_textarea_set_placeholder_text(ui_txpowerslider, "22");
      style_ta(ui_txpowerslider);

      // Right: SCREEN TIMEOUT
      lv_obj_t * to_col = lv_obj_create(pw_row);
      lv_obj_set_size(to_col, lv_pct(46), LV_SIZE_CONTENT);
      lv_obj_set_style_bg_opa(to_col, LV_OPA_TRANSP, 0);
      lv_obj_set_style_border_opa(to_col, LV_OPA_TRANSP, 0);
      lv_obj_set_style_pad_all(to_col, 0, 0);
      lv_obj_set_style_pad_row(to_col, 4, 0);
      lv_obj_set_flex_flow(to_col, LV_FLEX_FLOW_COLUMN);
      lv_obj_clear_flag(to_col, LV_OBJ_FLAG_SCROLLABLE);

      lv_obj_t * to_lbl = lv_label_create(to_col);
      lv_label_set_text(to_lbl, "TIMEOUT (seconds)");
      lv_obj_set_style_text_color(to_lbl, lv_color_hex(TH_ACCENT_LIGHT), 0);
      lv_obj_set_style_text_font(to_lbl, &lv_font_montserrat_14, 0);

      ui_screentimeout = lv_textarea_create(to_col);
      lv_obj_set_size(ui_screentimeout, lv_pct(100), 44);
      lv_textarea_set_one_line(ui_screentimeout, true);
      lv_textarea_set_accepted_chars(ui_screentimeout, "0123456789");
      lv_textarea_set_placeholder_text(ui_screentimeout, "30");
      style_ta(ui_screentimeout);
    }

    // ══════════════════════════════════════════════════════════
    // ADVERTISE
    // ══════════════════════════════════════════════════════════

    make_section_hdr(form, "ADVERTISE");

    lv_obj_t * adv_row = lv_obj_create(form);
    lv_obj_set_size(adv_row, lv_pct(100), 54);
    lv_obj_set_style_bg_opa(adv_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(adv_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(adv_row, 0, 0);
    lv_obj_set_style_pad_column(adv_row, 10, 0);
    lv_obj_set_flex_flow(adv_row, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(adv_row, LV_OBJ_FLAG_SCROLLABLE);

    ui_fadvertbutton    = make_action_btn(adv_row, &ui_Label9, BTN_HALF_W, 50);
    ui_zeroadvertbutton = make_action_btn(adv_row, &ui_Label8, BTN_HALF_W, 50);
    ui_positionadverttoggle = make_toggle_row(form, &ui_positionadvert_lbl, LV_SYMBOL_GPS " Include position in adverts");

    // ══════════════════════════════════════════════════════════
    // BRIGHTNESS
    // ══════════════════════════════════════════════════════════

    make_section_hdr(form, "BRIGHTNESS");

    ui_brightnessslider = lv_slider_create(form);
    lv_obj_set_width(ui_brightnessslider, lv_pct(95));
    lv_slider_set_range(ui_brightnessslider, 0, 100);
    lv_slider_set_value(ui_brightnessslider, 100, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(ui_brightnessslider, lv_color_hex(TH_SURFACE2), LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui_brightnessslider, lv_color_hex(TH_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(ui_brightnessslider, lv_color_hex(TH_ACCENT_LIGHT), LV_PART_KNOB);

    // ══════════════════════════════════════════════════════════
    // SCREEN ORIENTATION
    // ══════════════════════════════════════════════════════════

    make_section_hdr(form, "SCREEN ORIENTATION");

    ui_orientationtoggle = make_action_btn(form, &ui_orientation_lbl, lv_pct(100), 48);

    // ══════════════════════════════════════════════════════════
    // TIMEZONE
    // ══════════════════════════════════════════════════════════

    make_section_hdr(form, "TIMEZONE");

    ui_timezonedropdown = lv_dropdown_create(form);
    lv_obj_set_width(ui_timezonedropdown, lv_pct(100));
    style_dd(ui_timezonedropdown);

    // ══════════════════════════════════════════════════════════
    // NOTIFICATIONS & SOUND
    // ══════════════════════════════════════════════════════════

    make_section_hdr(form, "NOTIFICATIONS & SOUND");

    {
      lv_obj_t * notif_row = lv_obj_create(form);
      lv_obj_set_size(notif_row, lv_pct(100), 54);
      lv_obj_set_style_bg_opa(notif_row, LV_OPA_TRANSP, 0);
      lv_obj_set_style_border_opa(notif_row, LV_OPA_TRANSP, 0);
      lv_obj_set_style_pad_all(notif_row, 0, 0);
      lv_obj_set_style_pad_column(notif_row, 10, 0);
      lv_obj_set_flex_flow(notif_row, LV_FLEX_FLOW_ROW);
      lv_obj_clear_flag(notif_row, LV_OBJ_FLAG_SCROLLABLE);

      ui_notificationstoggle = make_action_btn(notif_row, &ui_Label20, BTN_HALF_W, 50);
    }
    // g_speaker_btn will be created by display.cpp inside the same row

    // ══════════════════════════════════════════════════════════
    //══════════════════════════════════════════════════════════
    // DANGER ZONE
    // ══════════════════════════════════════════════════════════

    make_section_hdr(form, "DANGER ZONE");

    ui_purgedatabutton = make_action_btn(form, &ui_Label21, lv_pct(100), 48);
    lv_obj_set_style_bg_color(ui_purgedatabutton, lv_color_hex(TH_RED), 0);

    // ══════════════════════════════════════════════════════════
    // FEATURES & BRIDGES
    // ══════════════════════════════════════════════════════════

    // ── TAB BAR ─────────────────────────────────────────────

    ui_tabbar_create(ui_settingscreen, 3);

    // ── KEYBOARD ────────────────────────────────────────────

    ui_Keyboard2 = lv_keyboard_create(ui_settingscreen);
    lv_obj_set_align(ui_Keyboard2, LV_ALIGN_TOP_LEFT);
    lv_obj_set_size(ui_Keyboard2, SCR_W, KB_HEIGHT);
    lv_obj_set_pos(ui_Keyboard2, 0, KB_Y_OFFSET);
    lv_obj_add_flag(ui_Keyboard2, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(ui_Keyboard2, lv_color_hex(TH_SURFACE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui_Keyboard2, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui_Keyboard2, lv_color_hex(TH_SURFACE2),
                               LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui_Keyboard2, lv_color_hex(TH_TEXT),
                                 LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_Keyboard2, lv_color_hex(TH_ACCENT),
                               LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(ui_Keyboard2, lv_color_white(),
                                 LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_border_opa(ui_Keyboard2, LV_OPA_TRANSP,
                                 LV_PART_ITEMS | LV_STATE_DEFAULT);
}

void ui_settingscreen_screen_destroy(void) {
    if (ui_settingscreen) { lv_obj_del(ui_settingscreen); ui_settingscreen = NULL; }
}
