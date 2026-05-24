// ui_repeaterscreen.c — Dark repeater management screen
// 480 × 800 portrait, ESP32-S3, LVGL 8.3

#include "ui.h"
#include "ui_tabbar.h"

// ── Widget pointers ─────────────────────────────────────────

lv_obj_t * ui_repeaterscreen       = NULL;
lv_obj_t * ui_repeaterloginbutton  = NULL;
lv_obj_t * ui_Label3               = NULL;
lv_obj_t * ui_Label7               = NULL;
lv_obj_t * ui_neighboursbutton     = NULL;
lv_obj_t * ui_Label13              = NULL;
lv_obj_t * ui_repeateradvertbutton = NULL;
lv_obj_t * ui_Label14              = NULL;
lv_obj_t * ui_repeatersdropdown    = NULL;
lv_obj_t * ui_repeatermonitor      = NULL;
lv_obj_t * ui_rebootbutton         = NULL;
lv_obj_t * ui_Label26              = NULL;
lv_obj_t * ui_repeaterpassword     = NULL;
lv_obj_t * ui_Label17              = NULL;
lv_obj_t * ui_statusbutton         = NULL;
lv_obj_t * ui_Label19              = NULL;
lv_obj_t * ui_Keyboard3            = NULL;
lv_obj_t * ui_Label4               = NULL;
lv_obj_t * ui_repeatersearchfield  = NULL;

// Stubs — SquareLine-era symbols still referenced in display.cpp widget arrays.
lv_obj_t * ui_homebutton2      = NULL;
lv_obj_t * ui_homeabel2        = NULL;
lv_obj_t * ui_settingsbutton2  = NULL;
lv_obj_t * ui_settingslabel2   = NULL;
lv_obj_t * ui_backbutton1      = NULL;
lv_obj_t * ui_backlabel1       = NULL;
lv_obj_t * ui_Button2          = NULL;

// ── Helpers ─────────────────────────────────────────────────

static lv_obj_t * make_btn_v(lv_obj_t * parent, lv_obj_t ** lbl_out,
                              lv_coord_t w, lv_coord_t h, uint32_t bg_color) {
    lv_obj_t * btn = lv_btn_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_radius(btn, 12, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg_color), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(btn, 2, 0);
    lv_obj_t * lbl = lv_label_create(btn);
    lv_label_set_text(lbl, "");
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(lbl);
    if (lbl_out) *lbl_out = lbl;
    return btn;
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
    lv_obj_t * list = lv_dropdown_get_list(dd);
    if (list) {
        lv_obj_set_style_bg_color(list, lv_color_hex(TH_SURFACE2), 0);
        lv_obj_set_style_text_color(list, lv_color_hex(TH_TEXT), 0);
        lv_obj_set_style_border_color(list, lv_color_hex(TH_BORDER), 0);
        lv_obj_set_style_text_font(list, &lv_font_montserrat_16, 0);
    }
}

// Button column width — wide enough for full labels
#define BTN_COL_W  120

// ── Screen init ─────────────────────────────────────────────

void ui_repeaterscreen_screen_init(void) {
    ui_repeaterscreen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(ui_repeaterscreen, lv_color_hex(TH_BG), 0);
    lv_obj_set_style_bg_opa(ui_repeaterscreen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(ui_repeaterscreen, LV_OBJ_FLAG_SCROLLABLE);

    // ── HEADER BAR ──────────────────────────────────────────

    lv_obj_t * hdr = lv_obj_create(ui_repeaterscreen);
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
    lv_label_set_text(title, LV_SYMBOL_WIFI "  Repeaters");
    lv_obj_set_style_text_color(title, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_center(title);

    // ── SCROLLABLE BODY ─────────────────────────────────────

    lv_obj_t * body = lv_obj_create(ui_repeaterscreen);
    lv_obj_set_pos(body, 0, STATUS_H);
    lv_obj_set_size(body, SCR_W, SCR_H - STATUS_H - TAB_H);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(body, 0, 0);
    lv_obj_set_style_pad_left(body, 10, 0);
    lv_obj_set_style_pad_right(body, 10, 0);
    lv_obj_set_style_pad_top(body, 6, 0);
    lv_obj_set_style_pad_bottom(body, 16, 0);
    lv_obj_set_style_pad_row(body, 6, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLL_ELASTIC);

    // ── Search field ────────────────────────────────────────

    ui_repeatersearchfield = lv_textarea_create(body);
    lv_obj_set_size(ui_repeatersearchfield, lv_pct(100), 40);
    lv_textarea_set_one_line(ui_repeatersearchfield, true);
    lv_textarea_set_placeholder_text(ui_repeatersearchfield,
                                      LV_SYMBOL_EYE_OPEN " Filter repeaters...");
    style_ta(ui_repeatersearchfield);

    // ── Repeater dropdown ───────────────────────────────────

    ui_repeatersdropdown = lv_dropdown_create(body);
    lv_obj_set_width(ui_repeatersdropdown, lv_pct(100));
    lv_dropdown_set_options(ui_repeatersdropdown, "(no repeaters found)");
    style_dd(ui_repeatersdropdown);

    // ── Password ────────────────────────────────────────────

    ui_repeaterpassword = lv_textarea_create(body);
    lv_obj_set_size(ui_repeaterpassword, lv_pct(100), 40);
    lv_textarea_set_one_line(ui_repeaterpassword, true);
    lv_textarea_set_password_mode(ui_repeaterpassword, true);
    lv_textarea_set_placeholder_text(ui_repeaterpassword,
                                      LV_SYMBOL_EDIT " Password...");
    style_ta(ui_repeaterpassword);

    // ── Login button ────────────────────────────────────────

    ui_repeaterloginbutton = lv_btn_create(body);
    lv_obj_set_size(ui_repeaterloginbutton, lv_pct(100), 44);
    lv_obj_set_style_radius(ui_repeaterloginbutton, 14, 0);
    lv_obj_set_style_bg_color(ui_repeaterloginbutton, lv_color_hex(TH_ACCENT), 0);
    lv_obj_set_style_bg_opa(ui_repeaterloginbutton, LV_OPA_COVER, 0);
    lv_obj_set_style_border_opa(ui_repeaterloginbutton, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_opa(ui_repeaterloginbutton, LV_OPA_TRANSP, 0);
    ui_Label3 = lv_label_create(ui_repeaterloginbutton);
    lv_label_set_text(ui_Label3, LV_SYMBOL_OK "  Login");
    lv_obj_set_style_text_color(ui_Label3, lv_color_white(), 0);
    lv_obj_set_style_text_font(ui_Label3, &lv_font_montserrat_16, 0);
    lv_obj_center(ui_Label3);

    // ══════════════════════════════════════════════════════════
    // STATUS MONITOR + ACTION BUTTONS — side by side
    //   Left:  monitor (flex-grow, expands to fit content)
    //   Right: vertical button column (BTN_COL_W wide)
    // ══════════════════════════════════════════════════════════

    lv_obj_t * mid_row = lv_obj_create(body);
    lv_obj_set_width(mid_row, lv_pct(100));
    lv_obj_set_height(mid_row, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(mid_row, 260, 0);
    lv_obj_set_style_bg_opa(mid_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(mid_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(mid_row, 0, 0);
    lv_obj_set_style_pad_column(mid_row, 6, 0);
    lv_obj_set_flex_flow(mid_row, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(mid_row, LV_OBJ_FLAG_SCROLLABLE);

    // ── Status monitor (left, grows to fill) ────────────────

    lv_obj_t * mon_wrap = lv_obj_create(mid_row);
    lv_obj_set_height(mon_wrap, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(mon_wrap, 260, 0);
    if (SCR_H < SCR_W) {
      lv_obj_set_width(mon_wrap, 400);
    } else {
      lv_obj_set_flex_grow(mon_wrap, 1);
    }
    lv_obj_set_style_bg_color(mon_wrap, lv_color_hex(0x0A0F14), 0);
    lv_obj_set_style_bg_opa(mon_wrap, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(mon_wrap, 1, 0);
    lv_obj_set_style_border_color(mon_wrap, lv_color_hex(TH_BORDER), 0);
    lv_obj_set_style_radius(mon_wrap, 8, 0);
    lv_obj_set_style_pad_all(mon_wrap, 8, 0);
    lv_obj_set_scroll_dir(mon_wrap, LV_DIR_VER);

    ui_repeatermonitor = lv_label_create(mon_wrap);
    lv_obj_set_width(ui_repeatermonitor, lv_pct(100));
    lv_obj_set_height(ui_repeatermonitor, LV_SIZE_CONTENT);
    lv_label_set_long_mode(ui_repeatermonitor, LV_LABEL_LONG_WRAP);
    lv_label_set_text(ui_repeatermonitor,
                       "Select a repeater and\nenter password to login.");
    lv_obj_set_style_text_color(ui_repeatermonitor, lv_color_hex(0xE0E0E0), 0);
    lv_obj_set_style_text_font(ui_repeatermonitor, &lv_font_montserrat_14, 0);

    // ── Action buttons column (right, BTN_COL_W wide) ───────

    lv_obj_t * btn_col = lv_obj_create(mid_row);
    if (SCR_H < SCR_W) {
      lv_obj_set_height(btn_col, LV_SIZE_CONTENT);
      lv_obj_set_flex_grow(btn_col, 1);
    } else {
      lv_obj_set_size(btn_col, BTN_COL_W, LV_SIZE_CONTENT);
    }
    lv_obj_set_style_bg_opa(btn_col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(btn_col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(btn_col, 0, 0);
    lv_obj_set_style_pad_row(btn_col, 6, 0);
    lv_obj_set_flex_flow(btn_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(btn_col, LV_OBJ_FLAG_SCROLLABLE);

    {
      lv_coord_t bcw = (SCR_H < SCR_W) ? lv_pct(96) : (BTN_COL_W - 4);
      int16_t    bch = (SCR_H < SCR_W) ? 48 : 56;
      ui_statusbutton         = make_btn_v(btn_col, &ui_Label19, bcw, bch, TH_ACCENT);
      ui_repeateradvertbutton = make_btn_v(btn_col, &ui_Label14, bcw, bch, TH_ACCENT);
      ui_neighboursbutton     = make_btn_v(btn_col, &ui_Label13, bcw, bch, TH_ACCENT);
      ui_rebootbutton         = make_btn_v(btn_col, &ui_Label26, bcw, bch, TH_RED);
    }

    lv_label_set_text(ui_Label19, LV_SYMBOL_REFRESH "\nStatus");
    lv_label_set_text(ui_Label14, LV_SYMBOL_UPLOAD  "\nAdvert");
    lv_label_set_text(ui_Label13, LV_SYMBOL_LIST    "\nNeighbours");
    lv_label_set_text(ui_Label26, LV_SYMBOL_POWER   "\nReboot");

    lv_obj_add_flag(ui_statusbutton,         LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_repeateradvertbutton, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_neighboursbutton,     LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_rebootbutton,         LV_OBJ_FLAG_HIDDEN);

    // Unused stubs
    ui_Label7  = NULL;
    ui_Label17 = NULL;
    ui_Label4  = NULL;

    // ── TAB BAR ─────────────────────────────────────────────

    ui_tabbar_create(ui_repeaterscreen, 1);

    // ── KEYBOARD ────────────────────────────────────────────

    ui_Keyboard3 = lv_keyboard_create(ui_repeaterscreen);
    lv_obj_set_align(ui_Keyboard3, LV_ALIGN_TOP_LEFT);
    lv_obj_set_size(ui_Keyboard3, SCR_W, KB_HEIGHT);
    lv_obj_set_pos(ui_Keyboard3, 0, KB_Y_OFFSET);
    lv_obj_add_flag(ui_Keyboard3, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(ui_Keyboard3, lv_color_hex(TH_SURFACE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui_Keyboard3, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui_Keyboard3, lv_color_hex(TH_SURFACE2),
                               LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui_Keyboard3, lv_color_hex(TH_TEXT),
                                 LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_Keyboard3, lv_color_hex(TH_ACCENT),
                               LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(ui_Keyboard3, lv_color_white(),
                                 LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_border_opa(ui_Keyboard3, LV_OPA_TRANSP,
                                 LV_PART_ITEMS | LV_STATE_DEFAULT);
}

void ui_repeaterscreen_screen_destroy(void) {
    if (ui_repeaterscreen) { lv_obj_del(ui_repeaterscreen); ui_repeaterscreen = NULL; }
}
