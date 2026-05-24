// ui_tabbar.c — Shared bottom tab bar for screen navigation

#include "ui_tabbar.h"
#include "ui_theme.h"
#include "ui_helpers.h"

// Forward-declare screens + init functions (avoids circular includes)
extern lv_obj_t * ui_homescreen;
extern lv_obj_t * ui_repeaterscreen;
extern lv_obj_t * ui_featuresscreen;
extern lv_obj_t * ui_settingscreen;
extern void ui_homescreen_screen_init(void);
extern void ui_repeaterscreen_screen_init(void);
extern void ui_featuresscreen_screen_init(void);
extern void ui_settingscreen_screen_init(void);
extern void features_register_callbacks(void);

// ── Navigation callbacks ────────────────────────────────────

static void tab_chat_cb(lv_event_t * e) {
    (void)e;
    _ui_screen_change(&ui_homescreen, LV_SCR_LOAD_ANIM_NONE, 0, 0,
                       ui_homescreen_screen_init);
}
static void tab_repeater_cb(lv_event_t * e) {
    (void)e;
    _ui_screen_change(&ui_repeaterscreen, LV_SCR_LOAD_ANIM_NONE, 0, 0,
                       ui_repeaterscreen_screen_init);
}
static void tab_webapps_cb(lv_event_t * e) {
    (void)e;
    _ui_screen_change(&ui_featuresscreen, LV_SCR_LOAD_ANIM_NONE, 0, 0,
                       ui_featuresscreen_screen_init);
    features_register_callbacks();
}
static void tab_settings_cb(lv_event_t * e) {
    (void)e;
    _ui_screen_change(&ui_settingscreen, LV_SCR_LOAD_ANIM_NONE, 0, 0,
                       ui_settingscreen_screen_init);
}

// ── Tab bar creation ────────────────────────────────────────

static lv_obj_t * make_tab_btn(lv_obj_t * bar, const char * icon,
                                const char * label, bool active,
                                lv_event_cb_t cb) {
    lv_obj_t * btn = lv_btn_create(bar);
    lv_obj_set_size(btn, SCR_W / 4, TAB_H);
    lv_obj_set_style_radius(btn, 0, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                           LV_FLEX_ALIGN_CENTER);

    uint32_t col = active ? TH_TAB_ACTIVE : TH_TAB_INACTIVE;

    // Active indicator (thin line at top of button)
    if (active) {
        lv_obj_t * ind = lv_obj_create(btn);
        lv_obj_set_size(ind, 36, 3);
        lv_obj_set_style_radius(ind, 2, 0);
        lv_obj_set_style_bg_color(ind, lv_color_hex(TH_TAB_ACTIVE), 0);
        lv_obj_set_style_bg_opa(ind, LV_OPA_COVER, 0);
        lv_obj_set_style_border_opa(ind, LV_OPA_TRANSP, 0);
        lv_obj_clear_flag(ind, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    }

    lv_obj_t * ic = lv_label_create(btn);
    lv_label_set_text(ic, icon);
    lv_obj_set_style_text_color(ic, lv_color_hex(col), 0);
    lv_obj_set_style_text_font(ic, &lv_font_montserrat_18, 0);

    lv_obj_t * lbl = lv_label_create(btn);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_color(lbl, lv_color_hex(col), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_10, 0);

    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    return btn;
}

void ui_tabbar_create(lv_obj_t * parent, int active_idx) {
    lv_obj_t * bar = lv_obj_create(parent);
    lv_obj_set_size(bar, SCR_W, TAB_H);
    lv_obj_set_pos(bar, 0, SCR_H - TAB_H);
    lv_obj_set_style_bg_color(bar, lv_color_hex(TH_TAB_BG), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(TH_SEPARATOR), 0);
    lv_obj_set_style_border_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_style_pad_column(bar, 0, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    make_tab_btn(bar, LV_SYMBOL_ENVELOPE,  "Chats",     active_idx == 0, tab_chat_cb);
    make_tab_btn(bar, LV_SYMBOL_LOOP,     "Repeaters", active_idx == 1, tab_repeater_cb);
    make_tab_btn(bar, LV_SYMBOL_WIFI,     "Web Apps",  active_idx == 2, tab_webapps_cb);
    make_tab_btn(bar, LV_SYMBOL_SETTINGS, "Settings",  active_idx == 3, tab_settings_cb);
}