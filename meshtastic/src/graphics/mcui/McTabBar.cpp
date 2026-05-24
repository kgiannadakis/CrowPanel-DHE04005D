#if HAS_TFT && USE_MCUI

#include "McTabBar.h"
#include "McTheme.h"
#include "McUI.h"

namespace mcui {

static constexpr int NUM_TABS = 4;

struct TabDef {
    const char *icon;
    const char *label;
};

static const TabDef tabs[NUM_TABS] = {
    {LV_SYMBOL_ENVELOPE, "Chats"},
    {LV_SYMBOL_LIST, "Nodes"},
    {LV_SYMBOL_GPS, "Maps"},
    {LV_SYMBOL_SETTINGS, "Settings"},
};

static lv_obj_t *s_bar = nullptr;
static lv_obj_t *s_btn[NUM_TABS] = {nullptr};
static lv_obj_t *s_icon[NUM_TABS] = {nullptr};
static lv_obj_t *s_label[NUM_TABS] = {nullptr};
static lv_obj_t *s_marker[NUM_TABS] = {nullptr};
static int s_active = -1;

static void tab_click_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    mcui::switchTab(idx);
}

lv_obj_t *tabbar_create(lv_obj_t *parent)
{
    s_bar = lv_obj_create(parent);
    lv_obj_remove_style_all(s_bar);
    lv_obj_set_size(s_bar, SCR_W, TAB_H);
    lv_obj_set_pos(s_bar, 0, SCR_H - TAB_H);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(TH_TAB_BG), 0);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_bar, 0, 0);
    lv_obj_set_style_pad_all(s_bar, 0, 0);
    lv_obj_remove_flag(s_bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *sep = lv_obj_create(s_bar);
    lv_obj_remove_style_all(sep);
    lv_obj_set_size(sep, SCR_W, 1);
    lv_obj_set_pos(sep, 0, 0);
    lv_obj_set_style_bg_color(sep, lv_color_hex(TH_SEPARATOR), 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);

    const int tabW = SCR_W / NUM_TABS;
    for (int i = 0; i < NUM_TABS; i++) {
        s_btn[i] = lv_obj_create(s_bar);
        lv_obj_remove_style_all(s_btn[i]);
        lv_obj_set_size(s_btn[i], tabW, TAB_H);
        lv_obj_set_pos(s_btn[i], i * tabW, 0);
        lv_obj_set_style_bg_opa(s_btn[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_btn[i], 0, 0);
        lv_obj_set_style_pad_all(s_btn[i], 0, 0);
        lv_obj_remove_flag(s_btn[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(s_btn[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_btn[i], tab_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        s_marker[i] = lv_obj_create(s_btn[i]);
        lv_obj_remove_style_all(s_marker[i]);
        lv_obj_set_size(s_marker[i], 36, 3);
        lv_obj_align(s_marker[i], LV_ALIGN_TOP_MID, 0, 4);
        lv_obj_set_style_bg_color(s_marker[i], lv_color_hex(TH_TAB_ACTIVE), 0);
        lv_obj_set_style_bg_opa(s_marker[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_radius(s_marker[i], 0, 0);

        s_icon[i] = lv_label_create(s_btn[i]);
        lv_label_set_text(s_icon[i], tabs[i].icon);
        lv_obj_set_style_text_color(s_icon[i], lv_color_hex(TH_TAB_INACTIVE), 0);
        lv_obj_set_style_text_font(s_icon[i], &lv_font_montserrat_16, 0);
        lv_obj_align(s_icon[i], LV_ALIGN_CENTER, 0, -8);

        s_label[i] = lv_label_create(s_btn[i]);
        lv_label_set_text(s_label[i], tabs[i].label);
        lv_obj_set_style_text_color(s_label[i], lv_color_hex(TH_TAB_INACTIVE), 0);
        lv_obj_align(s_label[i], LV_ALIGN_CENTER, 0, 14);
    }

    return s_bar;
}

void tabbar_set_active(int idx)
{
    if (idx < 0 || idx >= NUM_TABS) return;
    if (s_active == idx) return;
    if (s_active >= 0) {
        lv_obj_set_style_text_color(s_icon[s_active], lv_color_hex(TH_TAB_INACTIVE), 0);
        lv_obj_set_style_text_color(s_label[s_active], lv_color_hex(TH_TAB_INACTIVE), 0);
        lv_obj_set_style_bg_opa(s_marker[s_active], LV_OPA_TRANSP, 0);
    }
    lv_obj_set_style_text_color(s_icon[idx], lv_color_hex(TH_TAB_ACTIVE), 0);
    lv_obj_set_style_text_color(s_label[idx], lv_color_hex(TH_TAB_ACTIVE), 0);
    lv_obj_set_style_bg_opa(s_marker[idx], LV_OPA_COVER, 0);
    s_active = idx;
}

}

#endif
