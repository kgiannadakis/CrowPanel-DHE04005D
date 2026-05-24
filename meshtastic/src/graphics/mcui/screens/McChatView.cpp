#if HAS_TFT && USE_MCUI

#include "McChatView.h"
#include "../McKeyboard.h"
#include "../McStatusBar.h"
#include "../McTheme.h"
#include "../McUI.h"
#include "../data/McMessages.h"
#include "../data/McSender.h"
#include "configuration.h"

#include "mesh/NodeDB.h"

#include <cstdio>
#include <cstring>
#include <time.h>

namespace mcui {

static constexpr int HEADER_H = 44;

static constexpr int INPUT_ROW_H = 56;
static constexpr int KB_GAP = 8;
static int chat_top_y() { return landscape_active() ? 0 : STATUS_H; }
static int chat_h() { return landscape_active() ? (SCR_H - TAB_H) : PAGE_H; }
static bool merged_landscape_header() { return landscape_active(); }

static lv_obj_t *s_root = nullptr;
static lv_obj_t *s_header = nullptr;
static lv_obj_t *s_title = nullptr;
static lv_obj_t *s_back_btn = nullptr;
static lv_obj_t *s_time = nullptr;
static lv_obj_t *s_bubbles = nullptr;
static lv_obj_t *s_input_row = nullptr;
static lv_obj_t *s_textarea = nullptr;
static lv_obj_t *s_send_btn = nullptr;
static McConvId s_current = McConvId::none();
static uint32_t s_last_tick = 0;

static int16_t s_press_start_x = -1;

static constexpr int16_t EDGE_SWIPE_MARGIN = 40;

static void rebuild_bubbles();
static void refresh_header_time();
static void update_header_layout();

static void update_chat_frame()
{
    if (!s_root || !s_header || !s_bubbles)
        return;

    lv_obj_set_size(s_root, SCR_W, chat_h());
    lv_obj_set_pos(s_root, 0, chat_top_y());
    lv_obj_set_size(s_header, SCR_W, HEADER_H);
    lv_obj_set_width(s_bubbles, SCR_W);
    if (s_input_row)
        lv_obj_set_width(s_input_row, SCR_W);
}

static void layout_for_keyboard(bool kb_visible)
{
    if (!s_root || !s_bubbles || !s_input_row) return;

    int input_y_screen;
    if (kb_visible) {

        input_y_screen = SCR_H - keyboard_height() - INPUT_ROW_H - KB_GAP;
    } else {

        input_y_screen = SCR_H - TAB_H - INPUT_ROW_H;
    }
    int input_y_local = input_y_screen - chat_top_y();
    lv_obj_set_pos(s_input_row, 0, input_y_local);

    int bubble_h = input_y_local - HEADER_H;
    if (bubble_h < 0) bubble_h = 0;
    lv_obj_set_pos(s_bubbles, 0, HEADER_H);
    lv_obj_set_height(s_bubbles, bubble_h);
}

static void back_cb(lv_event_t *)
{
    chatview_hide();
}

static void refresh_header_time()
{
    if (!s_time)
        return;

    if (!merged_landscape_header()) {
        lv_obj_add_flag(s_time, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    char buf[8] = "--:--";
    time_t now = time(nullptr);
    if (now >= 1700000000) {
        struct tm lt;
        localtime_r(&now, &lt);
        snprintf(buf, sizeof(buf), "%02d:%02d", lt.tm_hour, lt.tm_min);
    }
    lv_label_set_text(s_time, buf);
    lv_obj_remove_flag(s_time, LV_OBJ_FLAG_HIDDEN);
}

static void update_header_layout()
{
    if (!s_header || !s_back_btn || !s_title || !s_time)
        return;

    if (merged_landscape_header()) {
        lv_obj_align(s_back_btn, LV_ALIGN_LEFT_MID, 12, 0);
        lv_obj_align(s_title, LV_ALIGN_CENTER, 0, 0);
        lv_obj_align(s_time, LV_ALIGN_RIGHT_MID, -12, 0);
        lv_obj_remove_flag(s_time, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_align(s_back_btn, LV_ALIGN_LEFT_MID, 12, 0);
        lv_obj_align(s_title, LV_ALIGN_CENTER, 0, 0);
        lv_obj_add_flag(s_time, LV_OBJ_FLAG_HIDDEN);
    }
}

static void press_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    s_press_start_x = (int16_t)p.x;

    if (keyboard_is_visible()) {
        keyboard_hide();
        layout_for_keyboard(false);
    }
}

static void press_input_cb(lv_event_t *)
{
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    s_press_start_x = (int16_t)p.x;
}

static void gesture_cb(lv_event_t *)
{
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir != LV_DIR_LEFT) return;

    if (s_press_start_x < 0 || s_press_start_x < (SCR_W - EDGE_SWIPE_MARGIN)) return;

    s_press_start_x = -1;

    lv_indev_wait_release(indev);
    chatview_hide();
}

static void ta_focus_cb(lv_event_t *)
{
    keyboard_attach(s_textarea);
    keyboard_show();
    layout_for_keyboard(true);

    if (s_bubbles) {
        lv_obj_update_layout(s_bubbles);
        lv_obj_scroll_to_y(s_bubbles, LV_COORD_MAX, LV_ANIM_OFF);
    }
}

static void send_cb(lv_event_t *)
{
    const char *txt = lv_textarea_get_text(s_textarea);
    if (!txt || !txt[0]) return;
    if (!s_current.is_valid()) return;
    if (sender_send_text(s_current, txt)) {
        lv_textarea_set_text(s_textarea, "");

    }
}

static void add_bubble(const McMessage &m)
{
    lv_obj_t *row = lv_obj_create(s_bubbles);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(row, 4, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_add_flag(row, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t *bubble = lv_obj_create(row);
    lv_obj_remove_style_all(bubble);
    lv_obj_add_flag(bubble, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_width(bubble, lv_pct(82));
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(bubble, 0, 0);
    lv_obj_set_style_pad_all(bubble, 10, 0);
    lv_obj_set_style_pad_row(bubble, 2, 0);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bubble, 0, 0);
    lv_obj_set_flex_flow(bubble, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);

    if (m.outgoing) {
        lv_obj_set_style_bg_color(bubble, lv_color_hex(TH_BUBBLE_OUT), 0);
        lv_obj_align(bubble, LV_ALIGN_RIGHT_MID, 0, 0);
    } else {
        lv_obj_set_style_bg_color(bubble, lv_color_hex(TH_BUBBLE_IN), 0);
        lv_obj_align(bubble, LV_ALIGN_LEFT_MID, 0, 0);
    }

    char head[64] = {0};
    if (!m.outgoing) {
        const char *sender = "?";
        if (nodeDB) {
            auto *n = nodeDB->getMeshNode(m.from_node);
            if (n && n->has_user) {
                sender = n->user.short_name[0] ? n->user.short_name : n->user.long_name;
            }
        }
        char ts[8] = "--:--";
        if (m.timestamp > 1700000000) {
            struct tm lt; time_t t = m.timestamp; localtime_r(&t, &lt);
            snprintf(ts, sizeof(ts), "%02d:%02d", lt.tm_hour, lt.tm_min);
        }
        snprintf(head, sizeof(head), "%s  %s", sender, ts);
        lv_obj_t *h = lv_label_create(bubble);
        lv_label_set_text(h, head);
        lv_obj_set_style_text_color(h, lv_color_hex(TH_ACCENT_LIGHT), 0);
    }

    lv_obj_t *body = lv_label_create(bubble);
    lv_label_set_text(body, m.text);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(body, lv_pct(100));
    lv_obj_set_style_text_color(body, lv_color_hex(TH_TEXT), 0);

    char foot[48] = {0};
    uint32_t foot_color = TH_TEXT3;
    if (m.outgoing) {

        if (!m.delivered) {
            snprintf(foot, sizeof(foot), "sent");
        } else if (m.ack_failed) {
            snprintf(foot, sizeof(foot), LV_SYMBOL_CLOSE " failed");
            foot_color = 0xE06060;
        } else if (m.packet_id == 0) {

            snprintf(foot, sizeof(foot), "sent");
        } else {

            snprintf(foot, sizeof(foot), LV_SYMBOL_OK LV_SYMBOL_OK " acknowledged");
        }
    } else if (m.snr != 0 || m.rssi != 0) {
        snprintf(foot, sizeof(foot), "SNR %.1f  RSSI %d", m.snr, (int)m.rssi);
    }
    if (foot[0]) {
        lv_obj_t *f = lv_label_create(bubble);
        lv_label_set_text(f, foot);
        lv_obj_set_style_text_color(f, lv_color_hex(foot_color), 0);

        lv_obj_set_style_text_font(f, &lv_font_montserrat_16, 0);
    }
}

static void rebuild_bubbles()
{
    if (!s_bubbles) return;
    lv_obj_clean(s_bubbles);
    if (!s_current.is_valid()) return;

    McMessage buf[MC_MAX_MSGS_PER_CONV];
    size_t n = messages_snapshot(s_current, buf, MC_MAX_MSGS_PER_CONV);
    for (size_t i = 0; i < n; i++) add_bubble(buf[i]);

    lv_obj_scroll_to_y(s_bubbles, LV_COORD_MAX, LV_ANIM_OFF);
}

lv_obj_t *chatview_create(lv_obj_t *parent)
{
    s_root = lv_obj_create(parent);
    lv_obj_remove_style_all(s_root);

    lv_obj_set_size(s_root, SCR_W, chat_h());
    lv_obj_set_pos(s_root, 0, chat_top_y());
    lv_obj_set_style_bg_color(s_root, lv_color_hex(TH_BG), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);

    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(s_root, gesture_cb, LV_EVENT_GESTURE, nullptr);

    s_header = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_header);
    lv_obj_set_size(s_header, SCR_W, HEADER_H);
    lv_obj_set_pos(s_header, 0, 0);
    lv_obj_set_style_bg_color(s_header, lv_color_hex(TH_SURFACE), 0);
    lv_obj_set_style_bg_opa(s_header, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_header, LV_OBJ_FLAG_SCROLLABLE);

    s_back_btn = lv_label_create(s_header);
    lv_label_set_text(s_back_btn, LV_SYMBOL_LEFT " Back");
    lv_obj_set_style_text_color(s_back_btn, lv_color_hex(TH_ACCENT), 0);
    lv_obj_align(s_back_btn, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_add_flag(s_back_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_back_btn, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(s_back_btn, back_cb, LV_EVENT_CLICKED, nullptr);

    s_title = lv_label_create(s_header);
    lv_label_set_text(s_title, "");
    lv_obj_set_style_text_color(s_title, lv_color_hex(TH_TEXT), 0);
    lv_obj_align(s_title, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(s_title, LV_OBJ_FLAG_EVENT_BUBBLE);

    s_time = lv_label_create(s_header);
    lv_label_set_text(s_time, "--:--");
    lv_obj_set_style_text_color(s_time, lv_color_hex(TH_TEXT2), 0);
    lv_obj_add_flag(s_time, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_add_event_cb(s_header, press_cb, LV_EVENT_PRESSED, nullptr);

    s_bubbles = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_bubbles);
    lv_obj_set_width(s_bubbles, SCR_W);
    lv_obj_set_style_bg_color(s_bubbles, lv_color_hex(TH_BG), 0);
    lv_obj_set_style_bg_opa(s_bubbles, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_bubbles, 8, 0);
    lv_obj_set_style_pad_row(s_bubbles, 6, 0);
    lv_obj_set_flex_flow(s_bubbles, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_bubbles, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_bubbles, LV_SCROLLBAR_MODE_AUTO);

    lv_obj_add_event_cb(s_bubbles, press_cb, LV_EVENT_PRESSED, nullptr);

    s_input_row = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_input_row);
    lv_obj_set_size(s_input_row, SCR_W, INPUT_ROW_H);
    lv_obj_set_style_bg_color(s_input_row, lv_color_hex(TH_SURFACE), 0);
    lv_obj_set_style_bg_opa(s_input_row, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_input_row, 6, 0);
    lv_obj_remove_flag(s_input_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_add_event_cb(s_input_row, press_input_cb, LV_EVENT_PRESSED, nullptr);

    constexpr int SEND_BTN_W = 80;
    constexpr int SEND_BTN_MARGIN = 6;
    constexpr int TA_MARGIN = 6;

    s_textarea = lv_textarea_create(s_input_row);
    lv_obj_set_size(s_textarea,
                    SCR_W - TA_MARGIN - SEND_BTN_W - SEND_BTN_MARGIN * 2,
                    INPUT_ROW_H - 12);
    lv_obj_set_pos(s_textarea, TA_MARGIN, 6);
    lv_textarea_set_one_line(s_textarea, true);
    lv_textarea_set_placeholder_text(s_textarea, "Message...");
    lv_obj_set_style_bg_color(s_textarea, lv_color_hex(TH_INPUT), 0);
    lv_obj_set_style_text_color(s_textarea, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_border_width(s_textarea, 0, 0);
    lv_obj_set_style_radius(s_textarea, 0, 0);

    lv_obj_set_style_anim_duration(s_textarea, 0, LV_PART_CURSOR);

    lv_obj_add_flag(s_textarea, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(s_textarea, ta_focus_cb, LV_EVENT_FOCUSED, nullptr);

    lv_obj_add_event_cb(s_textarea, send_cb, LV_EVENT_READY, nullptr);

    s_send_btn = lv_button_create(s_input_row);
    lv_obj_set_size(s_send_btn, SEND_BTN_W, INPUT_ROW_H - 12);
    lv_obj_set_pos(s_send_btn, SCR_W - SEND_BTN_W - SEND_BTN_MARGIN, 6);
    lv_obj_set_style_bg_color(s_send_btn, lv_color_hex(TH_ACCENT), 0);
    lv_obj_set_style_radius(s_send_btn, 0, 0);

    lv_obj_add_event_cb(s_send_btn, send_cb, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_flag(s_send_btn, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_t *sl = lv_label_create(s_send_btn);
    lv_label_set_text(sl, LV_SYMBOL_OK " Send");
    lv_obj_set_style_text_color(sl, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(sl, &lv_font_montserrat_16, 0);
    lv_obj_center(sl);

    update_chat_frame();
    update_header_layout();
    refresh_header_time();

    layout_for_keyboard(false);

    return s_root;
}

void chatview_open(const McConvId &id, const char *title)
{
    if (!s_root) return;
    s_current = id;
    lv_label_set_text(s_title, title ? title : "");
    lv_textarea_set_text(s_textarea, "");
    statusbar_set_visible(!merged_landscape_header());
    update_chat_frame();
    update_header_layout();
    refresh_header_time();
    rebuild_bubbles();
    messages_mark_read(id);
    s_last_tick = messages_change_tick();
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_root);

    keyboard_attach(s_textarea);
    keyboard_show();
    layout_for_keyboard(true);

    if (s_bubbles) {
        lv_obj_update_layout(s_bubbles);
        lv_obj_scroll_to_y(s_bubbles, LV_COORD_MAX, LV_ANIM_OFF);
    }
}

void chatview_hide()
{
    if (!s_root) return;
    keyboard_hide();
    statusbar_set_visible(true);
    update_chat_frame();
    update_header_layout();
    layout_for_keyboard(false);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    s_current = McConvId::none();
}

bool chatview_is_open()
{
    return s_root && !lv_obj_has_flag(s_root, LV_OBJ_FLAG_HIDDEN);
}

void chatview_tick()
{
    if (!chatview_is_open()) return;
    refresh_header_time();
    uint32_t t = messages_change_tick();
    if (t != s_last_tick) {
        s_last_tick = t;
        rebuild_bubbles();
        if (s_current.is_valid()) messages_mark_read(s_current);
    }
}

McConvId chatview_current() { return s_current; }

}

#endif
