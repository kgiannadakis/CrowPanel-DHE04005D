#if HAS_TFT && USE_MCUI

#include "McStatusBar.h"
#include "McTheme.h"
#include "McUI.h"
#include "configuration.h"
#include "mesh/NodeDB.h"

#include <cstring>
#include <time.h>

namespace mcui {

static lv_obj_t *s_bar = nullptr;
static lv_obj_t *s_name = nullptr;
static lv_obj_t *s_time = nullptr;
static char s_last_time_text[16] = "";

lv_obj_t *statusbar_create(lv_obj_t *parent)
{
    s_bar = lv_obj_create(parent);
    lv_obj_remove_style_all(s_bar);
    lv_obj_set_size(s_bar, SCR_W, STATUS_H);
    lv_obj_set_pos(s_bar, 0, 0);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(TH_SURFACE), 0);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_bar, 0, 0);
    lv_obj_set_style_pad_hor(s_bar, 14, 0);
    lv_obj_set_style_pad_ver(s_bar, 0, 0);
    lv_obj_remove_flag(s_bar, LV_OBJ_FLAG_SCROLLABLE);

    s_name = lv_label_create(s_bar);
    const char *name = (owner.short_name[0]) ? owner.short_name : "mesh";
    lv_label_set_text(s_name, name);
    lv_obj_set_style_text_color(s_name, lv_color_hex(TH_TEXT), 0);
    lv_obj_align(s_name, LV_ALIGN_LEFT_MID, 0, 0);

    s_time = lv_label_create(s_bar);
    lv_label_set_text(s_time, "--:--");
    strncpy(s_last_time_text, "--:--", sizeof(s_last_time_text) - 1);
    lv_obj_set_style_text_color(s_time, lv_color_hex(TH_TEXT2), 0);
    lv_obj_align(s_time, LV_ALIGN_RIGHT_MID, 0, 0);

    lv_obj_t *sep = lv_obj_create(s_bar);
    lv_obj_remove_style_all(sep);
    lv_obj_set_size(sep, SCR_W, 1);
    lv_obj_set_pos(sep, 0, STATUS_H - 1);
    lv_obj_set_style_bg_color(sep, lv_color_hex(TH_SEPARATOR), 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);

    return s_bar;
}

void statusbar_refresh()
{
    if (!s_time) return;
    char buf[16];
    time_t now = time(nullptr);
    if (now < 1700000000) {
        snprintf(buf, sizeof(buf), "--:--");
    } else {
        struct tm lt;
        localtime_r(&now, &lt);
        snprintf(buf, sizeof(buf), "%02d:%02d", lt.tm_hour, lt.tm_min);
    }
    if (strcmp(buf, s_last_time_text) == 0)
        return;
    lv_label_set_text(s_time, buf);
    strncpy(s_last_time_text, buf, sizeof(s_last_time_text) - 1);
    s_last_time_text[sizeof(s_last_time_text) - 1] = '\0';
}

void statusbar_set_visible(bool visible)
{
    if (!s_bar)
        return;
    if (visible)
        lv_obj_remove_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
}

}

#endif
