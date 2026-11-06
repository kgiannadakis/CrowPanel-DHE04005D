#include "ui_battery.h"
#include "ui_theme.h"
#include <stdio.h>

typedef struct {
    lv_obj_t* group;
    lv_obj_t* body;
    lv_obj_t* fill;
    lv_obj_t* label;
} BatteryWidgets;

static BatteryWidgets s_widgets[8];
static int s_percent = -1;
static uint32_t s_color = TH_TEXT2;

static void apply_reading(BatteryWidgets* w) {
    if (!w->group) return;
    char text[8];
    if (s_percent < 0) snprintf(text, sizeof(text), "--%%");
    else snprintf(text, sizeof(text), "%d%%", s_percent);
    lv_label_set_text(w->label, text);
    lv_obj_set_style_text_color(w->label, lv_color_hex(s_color), 0);
    int width = s_percent < 0 ? 0 : s_percent * 20 / 100;
    if (s_percent > 0 && width == 0) width = 1;
    lv_obj_set_width(w->fill, width);
    lv_obj_set_style_bg_color(w->fill, lv_color_hex(s_color), 0);
    lv_obj_clear_flag(w->group, LV_OBJ_FLAG_HIDDEN);
}

static void indicator_deleted(lv_event_t* e) {
    BatteryWidgets* w = lv_event_get_user_data(e);
    *w = (BatteryWidgets){0};
}

void ui_battery_create(lv_obj_t* header) {
    BatteryWidgets* w = NULL;
    for (unsigned i = 0; i < sizeof(s_widgets) / sizeof(s_widgets[0]); ++i) {
        if (!s_widgets[i].group) { w = &s_widgets[i]; break; }
    }
    if (!w) return;

    w->group = lv_obj_create(header);
    lv_obj_remove_style_all(w->group);
    lv_obj_set_size(w->group, 74, 20);
    // Absolute x=56 on every header, clear of the Chats back button. Header
    // padding differs between tabs, so compensate instead of shifting on nav.
    lv_obj_align(w->group, LV_ALIGN_LEFT_MID,
                 56 - lv_obj_get_style_pad_left(header, LV_PART_MAIN), 0);
    lv_obj_clear_flag(w->group, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(w->group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(w->group, indicator_deleted, LV_EVENT_DELETE, w);

    w->body = lv_obj_create(w->group);
    lv_obj_remove_style_all(w->body);
    lv_obj_set_size(w->body, 24, 12);
    lv_obj_align(w->body, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_radius(w->body, 2, 0);
    lv_obj_set_style_border_width(w->body, 1, 0);
    lv_obj_set_style_border_color(w->body, lv_color_hex(TH_TEXT2), 0);
    lv_obj_clear_flag(w->body, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(w->body, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    lv_obj_t* nub = lv_obj_create(w->body);
    lv_obj_remove_style_all(nub);
    lv_obj_set_size(nub, 2, 6);
    lv_obj_align(nub, LV_ALIGN_RIGHT_MID, 3, 0);
    lv_obj_set_style_bg_color(nub, lv_color_hex(TH_TEXT2), 0);
    lv_obj_set_style_bg_opa(nub, LV_OPA_COVER, 0);
    lv_obj_clear_flag(nub, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    w->fill = lv_obj_create(w->body);
    lv_obj_remove_style_all(w->fill);
    lv_obj_set_size(w->fill, 0, 8);
    lv_obj_align(w->fill, LV_ALIGN_LEFT_MID, 1, 0);
    lv_obj_set_style_radius(w->fill, 1, 0);
    lv_obj_set_style_bg_opa(w->fill, LV_OPA_COVER, 0);
    lv_obj_clear_flag(w->fill, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    w->label = lv_label_create(w->group);
    lv_label_set_text(w->label, "");
    lv_obj_set_style_text_font(w->label, &lv_font_montserrat_14, 0);
    lv_obj_align(w->label, LV_ALIGN_LEFT_MID, 32, 0);
    apply_reading(w);
}

void ui_battery_update(int percent, uint32_t color) {
    if (percent < 0 || percent > 100) return;
    if (percent == s_percent && color == s_color) return;
    s_percent = percent;
    s_color = color;
    for (unsigned i = 0; i < sizeof(s_widgets) / sizeof(s_widgets[0]); ++i)
        apply_reading(&s_widgets[i]);
}
