#if HAS_TFT && USE_MCUI

#include "McStatusBar.h"
#include "McTheme.h"
#include "McUI.h"
#include "configuration.h"
#include "mesh/NodeDB.h"

#include <cstring>
#include <time.h>

#if defined(CROWPANEL_DHE04005D)
#include <esp_err.h>
// Battery info from the onboard STC8 MCU (variants/.../stc8.cpp). Forward-declared
// here rather than including the board header, to keep the UI layer decoupled.
extern "C" esp_err_t stc8_read_battery(uint8_t *level_pct, uint8_t *charge_state, uint16_t *millivolts);
static constexpr uint8_t kStc8BatCharging = 1;     // STC8_BAT_CHARGING
static constexpr uint8_t kStc8BatFullyCharged = 2; // STC8_BAT_FULLY_CHARGED

// Battery % from the measured voltage using a piecewise-linear Li-ion
// resting-voltage -> state-of-charge curve, anchored to this pack: 4.10 V = 100%,
// 3.10 V = 0%. Unlike a straight line it follows the real cell shape: a quick
// drop off full, a long flat plateau through the middle (most capacity sits
// between ~3.7 and 3.9 V), then a steep knee near empty. Tune the points to your
// battery/charger if needed; the endpoints define the 100%/0% cutoffs.
static int batt_pct_from_mv(int mv)
{
    static const struct { int mv; int pct; } kCurve[] = {
        {4100, 100}, {4000, 92}, {3900, 85}, {3850, 78}, {3800, 68},
        {3750, 55},  {3700, 42}, {3650, 32}, {3600, 25}, {3500, 17},
        {3400, 11},  {3300, 6},  {3200, 3},  {3100, 0},
    };
    const int n = (int)(sizeof(kCurve) / sizeof(kCurve[0]));
    if (mv >= kCurve[0].mv) return 100;
    if (mv <= kCurve[n - 1].mv) return 0;
    for (int i = 1; i < n; i++) {
        if (mv >= kCurve[i].mv) {
            const int vlo = kCurve[i].mv,     plo = kCurve[i].pct;     // lower point
            const int vhi = kCurve[i - 1].mv, phi = kCurve[i - 1].pct; // upper point
            return plo + (mv - vlo) * (phi - plo) / (vhi - vlo);
        }
    }
    return 0;
}
#endif

namespace mcui {

static lv_obj_t *s_bar = nullptr;
static lv_obj_t *s_name = nullptr;
static lv_obj_t *s_time = nullptr;
static lv_obj_t *s_batt = nullptr;       // percentage label
static lv_obj_t *s_batt_body = nullptr;  // battery icon outline
static lv_obj_t *s_batt_fill = nullptr;  // battery icon fill (width tracks %)
static char s_last_time_text[16] = "";

#if defined(CROWPANEL_DHE04005D)
// Drawn battery-icon geometry (px). Font-independent, so it works with any
// theme font. The fill sits inside the body and its width tracks the level.
static constexpr int kBattBodyW = 24;
static constexpr int kBattBodyH = 12;
static constexpr int kBattFillMaxW = kBattBodyW - 4; // inside the 1px border + inset
static constexpr int kBattFillH = 8;

// Cache written by statusbar_poll_battery() (no LVGL, no lock) and consumed by
// statusbar_refresh() (LVGL, under lock). Keeps the blocking I2C read off the
// render/touch path.
static bool s_batt_dirty = false;
static bool s_batt_have = false;
static int s_batt_pct_cache = 0;
static uint32_t s_batt_col_cache = 0;
#endif

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

    // Battery indicator (from the onboard STC8), left of the clock: a drawn
    // battery icon plus a percentage label. Hidden until the first successful
    // read, so boards/units without battery reporting show nothing.
    s_batt = lv_label_create(s_bar);
    lv_label_set_text(s_batt, "");
    lv_obj_set_style_text_color(s_batt, lv_color_hex(TH_TEXT2), 0);
    lv_obj_align(s_batt, LV_ALIGN_RIGHT_MID, -58, 0);

#if defined(CROWPANEL_DHE04005D)
    // Icon body (rounded outline) left of the percentage label, with a small gap
    // to the number. Offset leaves clearance past the terminal nub even for a
    // 3-digit "100%".
    s_batt_body = lv_obj_create(s_bar);
    lv_obj_remove_style_all(s_batt_body);
    lv_obj_set_size(s_batt_body, kBattBodyW, kBattBodyH);
    lv_obj_align(s_batt_body, LV_ALIGN_RIGHT_MID, -104, 0);
    lv_obj_set_style_radius(s_batt_body, 2, 0);
    lv_obj_set_style_border_width(s_batt_body, 1, 0);
    lv_obj_set_style_border_color(s_batt_body, lv_color_hex(TH_TEXT2), 0);
    lv_obj_set_style_bg_opa(s_batt_body, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(s_batt_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_batt_body, LV_OBJ_FLAG_HIDDEN);

    // Positive terminal nub on the right end of the body.
    lv_obj_t *nub = lv_obj_create(s_batt_body);
    lv_obj_remove_style_all(nub);
    lv_obj_set_size(nub, 2, 6);
    lv_obj_align(nub, LV_ALIGN_RIGHT_MID, 3, 0);
    lv_obj_set_style_bg_color(nub, lv_color_hex(TH_TEXT2), 0);
    lv_obj_set_style_bg_opa(nub, LV_OPA_COVER, 0);

    // Fill bar inside the body; its width is set from the level at refresh.
    s_batt_fill = lv_obj_create(s_batt_body);
    lv_obj_remove_style_all(s_batt_fill);
    lv_obj_set_size(s_batt_fill, 0, kBattFillH);
    lv_obj_align(s_batt_fill, LV_ALIGN_LEFT_MID, 1, 0);
    lv_obj_set_style_radius(s_batt_fill, 1, 0);
    lv_obj_set_style_bg_color(s_batt_fill, lv_color_hex(TH_TEXT2), 0);
    lv_obj_set_style_bg_opa(s_batt_fill, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_batt_fill, LV_OBJ_FLAG_SCROLLABLE);
#endif

    lv_obj_t *sep = lv_obj_create(s_bar);
    lv_obj_remove_style_all(sep);
    lv_obj_set_size(sep, SCR_W, 1);
    lv_obj_set_pos(sep, 0, STATUS_H - 1);
    lv_obj_set_style_bg_color(sep, lv_color_hex(TH_SEPARATOR), 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);

    return s_bar;
}

void statusbar_poll_battery()
{
#if defined(CROWPANEL_DHE04005D)
    // Blocking I2C read of the STC8, at most every 5 s. Called WITHOUT the LVGL
    // lock so it never stalls the render/touch task; results are cached and
    // applied later by statusbar_refresh().
    static uint32_t s_last_batt_ms = 0;
    uint32_t nowm = millis();
    if (s_last_batt_ms != 0 && (uint32_t)(nowm - s_last_batt_ms) < 5000)
        return;
    s_last_batt_ms = nowm;

    uint8_t state = 0;
    uint16_t mv = 0;
    // Guard on a plausible battery range so a missing/uninitialised reading
    // (0 mV) leaves the indicator blank rather than showing 0%.
    if (stc8_read_battery(nullptr, &state, &mv) == ESP_OK && mv >= 2500 && mv <= 5000) {
        int pct = batt_pct_from_mv((int)mv);
        uint32_t col = TH_TEXT2;
        if (state == kStc8BatCharging || state == kStc8BatFullyCharged)
            col = 0x4CAF50; // green while charging / full
        else if (pct <= 15)
            col = 0xE06060; // red when low
        s_batt_pct_cache = pct;
        s_batt_col_cache = col;
        s_batt_have = true;
        s_batt_dirty = true;
    }
#endif
}

void statusbar_refresh()
{
#if defined(CROWPANEL_DHE04005D)
    // Apply the latest battery reading (gathered off-lock by statusbar_poll_battery).
    if (s_batt && s_batt_have && s_batt_dirty) {
        s_batt_dirty = false;
        char bb[8];
        snprintf(bb, sizeof(bb), "%d%%", s_batt_pct_cache);
        lv_label_set_text(s_batt, bb);
        lv_obj_set_style_text_color(s_batt, lv_color_hex(s_batt_col_cache), 0);
        if (s_batt_fill && s_batt_body) {
            int fill_w = s_batt_pct_cache * kBattFillMaxW / 100;
            if (fill_w < 1 && s_batt_pct_cache > 0) fill_w = 1; // keep a sliver visible
            lv_obj_set_width(s_batt_fill, fill_w);
            lv_obj_set_style_bg_color(s_batt_fill, lv_color_hex(s_batt_col_cache), 0);
            lv_obj_remove_flag(s_batt_body, LV_OBJ_FLAG_HIDDEN);
        }
    }
#endif

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
