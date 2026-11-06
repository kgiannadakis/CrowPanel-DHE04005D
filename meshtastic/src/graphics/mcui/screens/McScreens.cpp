#if HAS_TFT && USE_MCUI

#include "McScreens.h"
#include "MapTileLayout.h"
#include "McChatView.h"
#include "../McKeyboard.h"
#include "../McTheme.h"
#include "../McUI.h"
#include "../data/McMessages.h"

#include "configuration.h"
#include "mesh/Channels.h"
#include "mesh/MeshService.h"
#include "mesh/NodeDB.h"
#include "mesh/PositionCoordinates.h"
#include "mesh/Throttle.h"
#include "mesh/generated/meshtastic/channel.pb.h"

#include <Arduino.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <mbedtls/base64.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <draw/lv_image_decoder.h>
#if defined(ARCH_ESP32) && defined(CROWPANEL_DHE04005D) && !defined(CROWPANEL_DISABLE_MAPS)
#include <esp_err.h>
#include <esp_vfs_fat.h>
#include <driver/sdmmc_host.h>
#include <sdmmc_cmd.h>
#include "board_config.h"
#endif

namespace mcui {

static lv_obj_t *make_page(lv_obj_t *parent)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, SCR_W, PAGE_H);
    lv_obj_set_pos(p, 0, 0);
    lv_obj_set_style_bg_color(p, lv_color_hex(TH_BG), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_pad_all(p, 0, 0);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

static const uint32_t kAvatarPalette[TH_AVATAR_PALETTE_COUNT] = { TH_AVATAR_PALETTE_LIST };

static uint32_t avatar_color_for(const McConvId &id, const char *title)
{
    uint32_t h = 2166136261u ^ (uint32_t)id.kind ^ id.value;
    if (title) {
        for (const char *p = title; *p; p++) {
            h ^= (uint8_t)(unsigned char)*p;
            h *= 16777619u;
        }
    }
    return kAvatarPalette[h % TH_AVATAR_PALETTE_COUNT];
}

static bool channel_create(const char *name, const uint8_t *psk = nullptr, size_t psk_len = 0,
                           bool uplink_enabled = false, bool downlink_enabled = false)
{
    if (!name || !*name) return false;
    if (psk_len != 0 && psk_len != 1 && psk_len != 16 && psk_len != 32) {
        LOG_WARN("mcui: channel_create: psk_len=%u invalid (must be 0/1/16/32)", (unsigned)psk_len);
        return false;
    }

    int target = -1;
    for (uint8_t i = 1; i < channels.getNumChannels(); i++) {
        meshtastic_Channel &c = channels.getByIndex(i);
        if (c.role == meshtastic_Channel_Role_DISABLED) {
            target = i;
            break;
        }
    }
    if (target < 0) {
        LOG_WARN("mcui: channel_create: no free secondary slot");
        return false;
    }

    meshtastic_Channel ch = channels.getByIndex(target);
    ch.index           = target;
    ch.role            = meshtastic_Channel_Role_SECONDARY;
    ch.has_settings    = true;
    memset(&ch.settings, 0, sizeof(ch.settings));
    strncpy(ch.settings.name, name, sizeof(ch.settings.name) - 1);
    ch.settings.name[sizeof(ch.settings.name) - 1] = '\0';
    ch.settings.uplink_enabled = uplink_enabled;
    ch.settings.downlink_enabled = downlink_enabled;

    if (psk && psk_len > 0) {

        memcpy(ch.settings.psk.bytes, psk, psk_len);
        ch.settings.psk.size = psk_len;
    } else {

        ch.settings.psk.size = 16;
        esp_fill_random(ch.settings.psk.bytes, 16);
    }

    channels.setChannel(ch);
    channels.onConfigChanged();
    if (service)
        service->reloadConfig(SEGMENT_CHANNELS);
    else if (nodeDB)
        nodeDB->saveToDisk(SEGMENT_CHANNELS);

    LOG_INFO("mcui: %s channel '%s' in slot %d (psk_len=%u)",
             (psk && psk_len) ? "joined" : "created", name, target,
             (unsigned)ch.settings.psk.size);
    return true;
}

static bool channel_delete(uint8_t idx)
{
    if (idx == 0 || idx >= channels.getNumChannels()) return false;
    meshtastic_Channel ch = channels.getByIndex(idx);
    if (ch.role == meshtastic_Channel_Role_DISABLED) return false;

    LOG_INFO("mcui: deleting channel slot %d (name='%s')", idx, ch.settings.name);
    ch.role = meshtastic_Channel_Role_DISABLED;
    ch.has_settings = true;
    memset(&ch.settings, 0, sizeof(ch.settings));
    channels.setChannel(ch);
    channels.onConfigChanged();
    if (service)
        service->reloadConfig(SEGMENT_CHANNELS);
    else if (nodeDB)
        nodeDB->saveToDisk(SEGMENT_CHANNELS);
    return true;
}

static lv_obj_t *s_chats_page = nullptr;
static lv_obj_t *s_chats_list = nullptr;
static lv_obj_t *s_chat_delete_overlay = nullptr;
static uint32_t s_chats_last_tick = 0xFFFFFFFFu;
static McConvId s_pending_delete_id = McConvId::none();
static McConvId s_suppress_click_id = McConvId::none();
static uint32_t s_suppress_click_ms = 0;

struct ChatEntry {
    McConvId id;
    char title[40];
};
static ChatEntry s_entries[MC_MAX_CONVERSATIONS + 8];
static int s_num_entries = 0;

static void rebuild_chats_list();

static void chat_card_clicked(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_current_target(e);
    ChatEntry *ent = (ChatEntry *)lv_obj_get_user_data(obj);
    if (!ent) return;
    if (s_suppress_click_id.is_valid() && ent->id == s_suppress_click_id) {
        if ((uint32_t)(lv_tick_get() - s_suppress_click_ms) < 1200) {
            s_suppress_click_id = McConvId::none();
            return;
        }
        s_suppress_click_id = McConvId::none();
    }
    chatview_open(ent->id, ent->title);
}

static void chat_delete_overlay_close()
{
    if (s_chat_delete_overlay) {
        lv_obj_delete(s_chat_delete_overlay);
        s_chat_delete_overlay = nullptr;
    }
    s_pending_delete_id = McConvId::none();
}

static void chat_delete_confirm_cb(lv_event_t *e)
{
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_current_target(e);
    bool do_delete = (btn == (lv_obj_t *)lv_event_get_user_data(e));
    McConvId id = s_pending_delete_id;
    chat_delete_overlay_close();
    if (!do_delete) return;

    messages_delete_conv(id);

    if (id.kind == McConvId::CHANNEL && id.value > 0) {
        channel_delete((uint8_t)id.value);
    }

    rebuild_chats_list();
}

static void chat_card_long_pressed(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_current_target(e);
    ChatEntry *ent = (ChatEntry *)lv_obj_get_user_data(obj);
    if (!ent) return;

    if (ent->id.kind != McConvId::DIRECT && ent->id.kind != McConvId::CHANNEL) return;

    s_suppress_click_id = ent->id;
    s_suppress_click_ms = lv_tick_get();

    lv_indev_t *indev = lv_indev_active();
    if (indev)
        lv_indev_wait_release(indev);

    chat_delete_overlay_close();
    s_pending_delete_id = ent->id;

    const bool is_channel  = (ent->id.kind == McConvId::CHANNEL);
    const bool is_primary  = (is_channel && ent->id.value == 0);
    const bool is_secondary = (is_channel && ent->id.value > 0);

    char title[96];
    const char *body_text;
    const char *delete_label;
    if (is_primary) {
        snprintf(title, sizeof(title), "Clear chat history?\n%s", ent->title);
        body_text = "Deletes the message history for this primary channel. "
                    "The channel itself stays - you keep receiving messages on it.";
        delete_label = "Clear history";
    } else if (is_secondary) {
        snprintf(title, sizeof(title), "Delete channel?\n%s", ent->title);
        body_text = "Permanently removes this channel and all its messages. "
                    "You will stop sending and receiving on this channel.";
        delete_label = "Delete channel";
    } else {

        snprintf(title, sizeof(title), "Delete private chat?\n%s", ent->title);
        body_text = "This deletes only the message history. The node stays in your node list.";
        delete_label = "Delete chat";
    }

    lv_obj_t *scr = lv_screen_active();
    s_chat_delete_overlay = lv_obj_create(scr);
    lv_obj_remove_style_all(s_chat_delete_overlay);
    lv_obj_set_size(s_chat_delete_overlay, SCR_W, SCR_H);
    lv_obj_set_pos(s_chat_delete_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_chat_delete_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_chat_delete_overlay, LV_OPA_60, 0);
    lv_obj_remove_flag(s_chat_delete_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(s_chat_delete_overlay);

    lv_obj_t *card = lv_obj_create(s_chat_delete_overlay);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, SCR_W - 48, 250);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(TH_SURFACE), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 0, 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *tl = lv_label_create(card);
    lv_label_set_text(tl, title);
    lv_label_set_long_mode(tl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(tl, lv_pct(100));
    lv_obj_set_style_text_color(tl, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(tl, MCUI_FONT_16, 0);
    lv_obj_align(tl, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *body = lv_label_create(card);
    lv_label_set_text(body, body_text);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(body, lv_pct(100));
    lv_obj_set_style_text_color(body, lv_color_hex(TH_TEXT2), 0);
    lv_obj_set_style_text_font(body, MCUI_FONT_16, 0);
    lv_obj_align(body, LV_ALIGN_TOP_LEFT, 0, 64);

    lv_obj_t *cancel = lv_button_create(card);
    lv_obj_set_size(cancel, 150, 42);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(TH_INPUT), 0);
    lv_obj_set_style_radius(cancel, 0, 0);
    lv_obj_t *cl = lv_label_create(cancel);
    lv_label_set_text(cl, "Cancel");
    lv_obj_set_style_text_color(cl, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(cl, MCUI_FONT_16, 0);
    lv_obj_center(cl);

    lv_obj_t *delete_btn = lv_button_create(card);
    lv_obj_set_size(delete_btn, 150, 42);
    lv_obj_align(delete_btn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(delete_btn, lv_color_hex(0xB83232), 0);
    lv_obj_set_style_radius(delete_btn, 0, 0);
    lv_obj_t *dl = lv_label_create(delete_btn);
    lv_label_set_text(dl, delete_label);
    lv_obj_set_style_text_color(dl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(dl, MCUI_FONT_16, 0);
    lv_obj_center(dl);

    lv_obj_add_event_cb(cancel, chat_delete_confirm_cb, LV_EVENT_CLICKED, delete_btn);
    lv_obj_add_event_cb(delete_btn, chat_delete_confirm_cb, LV_EVENT_CLICKED, delete_btn);
}

static lv_obj_t *s_chcreate_overlay = nullptr;
static lv_obj_t *s_chcreate_name    = nullptr;
static lv_obj_t *s_chcreate_psk     = nullptr;
static lv_obj_t *s_chcreate_status  = nullptr;
static lv_obj_t *s_chcreate_uplink_sw = nullptr;
static lv_obj_t *s_chcreate_downlink_sw = nullptr;

static void chcreate_close()
{
    keyboard_hide();
    if (s_chcreate_overlay) {
        lv_obj_delete(s_chcreate_overlay);
        s_chcreate_overlay = nullptr;
    }
    s_chcreate_name   = nullptr;
    s_chcreate_psk    = nullptr;
    s_chcreate_status = nullptr;
    s_chcreate_uplink_sw = nullptr;
    s_chcreate_downlink_sw = nullptr;
}

static void chcreate_cancel_cb(lv_event_t *) { chcreate_close(); }

static void chcreate_status(const char *msg, bool ok)
{
    if (!s_chcreate_status) return;
    lv_label_set_text(s_chcreate_status, msg);
    lv_obj_set_style_text_color(s_chcreate_status,
                                lv_color_hex(ok ? 0x45D483 : 0xE05050), 0);
}

static size_t b64_clean(const char *in, char *out, size_t out_sz)
{
    size_t n = 0;
    if (!in) return 0;
    for (; *in && n + 1 < out_sz; in++) {
        char c = *in;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        out[n++] = c;
    }
    out[n] = '\0';
    return n;
}

static lv_obj_t *chcreate_add_switch_row(lv_obj_t *card, const char *label, int y, bool initial)
{
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), 44);
    lv_obj_align(row, LV_ALIGN_TOP_LEFT, 0, y);
    lv_obj_set_style_bg_color(row, lv_color_hex(TH_INPUT), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_pad_left(row, 10, 0);
    lv_obj_set_style_pad_right(row, 10, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_color(lbl, lv_color_hex(TH_TEXT2), 0);
    lv_obj_set_style_text_font(lbl, MCUI_FONT_16, 0);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 56, 28);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
    if (initial) lv_obj_add_state(sw, LV_STATE_CHECKED);
    return sw;
}

static void chcreate_ok_cb(lv_event_t *)
{
    if (!s_chcreate_name) { chcreate_close(); return; }

    const char *raw_name = lv_textarea_get_text(s_chcreate_name);
    if (!raw_name) { chcreate_close(); return; }
    char name[12] = {};
    int len = (int)strlen(raw_name);
    int s = 0;
    while (s < len && (raw_name[s] == ' ' || raw_name[s] == '\t')) s++;
    int e = len;
    while (e > s && (raw_name[e-1] == ' ' || raw_name[e-1] == '\t')) e--;
    int copy = e - s;
    if (copy > (int)sizeof(name) - 1) copy = sizeof(name) - 1;
    if (copy > 0) memcpy(name, raw_name + s, copy);
    name[copy] = '\0';
    if (!name[0]) {
        chcreate_status("Enter a channel name", false);
        return;
    }

    const uint8_t *psk_ptr = nullptr;
    size_t         psk_len = 0;
    uint8_t        psk_buf[32];

    if (s_chcreate_psk) {
        const char *raw_psk = lv_textarea_get_text(s_chcreate_psk);
        char cleaned[80];
        size_t clen = b64_clean(raw_psk, cleaned, sizeof(cleaned));
        if (clen > 0) {

            size_t olen = 0;
            int rc = mbedtls_base64_decode(psk_buf, sizeof(psk_buf), &olen,
                                           (const unsigned char *)cleaned, clen);
            if (rc != 0) {
                chcreate_status("PSK is not valid base64", false);
                return;
            }
            if (olen != 1 && olen != 16 && olen != 32) {
                chcreate_status("Decoded PSK must be 1, 16, or 32 bytes", false);
                return;
            }
            psk_ptr = psk_buf;
            psk_len = olen;
        }
    }

    bool uplink_enabled = s_chcreate_uplink_sw && lv_obj_has_state(s_chcreate_uplink_sw, LV_STATE_CHECKED);
    bool downlink_enabled = s_chcreate_downlink_sw && lv_obj_has_state(s_chcreate_downlink_sw, LV_STATE_CHECKED);

    if (!channel_create(name, psk_ptr, psk_len, uplink_enabled, downlink_enabled)) {
        chcreate_status("No free channel slot (delete one first)", false);
        return;
    }
    rebuild_chats_list();
    chcreate_close();
}

static void chcreate_focus_cb(lv_event_t *e)
{
    lv_obj_t *ta = (lv_obj_t *)lv_event_get_current_target(e);
    keyboard_attach(ta);
    keyboard_show();
}

// Hide the on-screen keyboard when the user taps anywhere in the modal that
// isn't a text field (card body, labels, switches, buttons, dimmed backdrop).
// Without this the keyboard stays up and can cover the Cancel / Create buttons,
// leaving no way out but a reboot. Mirrors the MQTT-settings modal behaviour.
static void chcreate_dismiss_kb_cb(lv_event_t *e)
{
    if (!keyboard_is_visible()) return;
    lv_obj_t *target = (lv_obj_t *)lv_event_get_target(e);
    if (target && lv_obj_check_type(target, &lv_textarea_class)) return;
    keyboard_hide();
}

static void chcreate_bind_dismiss_recursive(lv_obj_t *obj)
{
    if (!obj) return;
    if (!lv_obj_check_type(obj, &lv_textarea_class))
        lv_obj_add_event_cb(obj, chcreate_dismiss_kb_cb, LV_EVENT_CLICKED, nullptr);

    uint32_t child_count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < child_count; ++i)
        chcreate_bind_dismiss_recursive(lv_obj_get_child(obj, (int32_t)i));
}

static void chcreate_open()
{
    chcreate_close();

    lv_obj_t *scr = lv_screen_active();
    s_chcreate_overlay = lv_obj_create(scr);
    lv_obj_remove_style_all(s_chcreate_overlay);
    lv_obj_set_size(s_chcreate_overlay, SCR_W, SCR_H);
    lv_obj_set_pos(s_chcreate_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_chcreate_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_chcreate_overlay, LV_OPA_60, 0);
    lv_obj_remove_flag(s_chcreate_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(s_chcreate_overlay);

    int card_h = SCR_H - keyboard_height() - 24;
    if (card_h < 380) card_h = 380;
    if (card_h > 560) card_h = 560;
    lv_obj_t *card = lv_obj_create(s_chcreate_overlay);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, SCR_W - 40, card_h);
    lv_obj_set_pos(card, 20, 12);
    lv_obj_set_style_bg_color(card, lv_color_hex(TH_SURFACE), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 0, 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_set_scroll_dir(card, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_AUTO);

    int y = 0;

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "New / join channel");
    lv_obj_set_style_text_color(title, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(title, MCUI_FONT_16, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, y);
    y += 28;

    s_chcreate_name = lv_textarea_create(card);
    lv_obj_set_size(s_chcreate_name, lv_pct(100), 44);
    lv_obj_align(s_chcreate_name, LV_ALIGN_TOP_LEFT, 0, y);
    lv_textarea_set_one_line(s_chcreate_name, true);
    lv_textarea_set_max_length(s_chcreate_name, 11);
    lv_textarea_set_placeholder_text(s_chcreate_name, "Channel name (max 11 chars)");
    lv_obj_set_style_bg_color(s_chcreate_name, lv_color_hex(TH_INPUT), 0);
    lv_obj_set_style_text_color(s_chcreate_name, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(s_chcreate_name, MCUI_FONT_16, 0);
    lv_obj_set_style_border_width(s_chcreate_name, 0, 0);
    lv_obj_set_style_radius(s_chcreate_name, 0, 0);
    lv_obj_set_style_anim_duration(s_chcreate_name, 0, LV_PART_CURSOR);
    lv_obj_add_event_cb(s_chcreate_name, chcreate_focus_cb, LV_EVENT_FOCUSED, nullptr);
    y += 50;

    lv_obj_t *psk_hint = lv_label_create(card);

    lv_label_set_text(psk_hint,
                      "Key (base64). Blank = new random key. "
                      "Type a key to join an existing channel.");
    lv_label_set_long_mode(psk_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(psk_hint, lv_pct(100));
    lv_obj_set_style_text_color(psk_hint, lv_color_hex(TH_TEXT3), 0);
    lv_obj_set_style_text_font(psk_hint, MCUI_FONT_16, 0);
    lv_obj_align(psk_hint, LV_ALIGN_TOP_LEFT, 0, y);
    y += 56;

    s_chcreate_psk = lv_textarea_create(card);
    lv_obj_set_size(s_chcreate_psk, lv_pct(100), 44);
    lv_obj_align(s_chcreate_psk, LV_ALIGN_TOP_LEFT, 0, y);
    lv_textarea_set_one_line(s_chcreate_psk, true);
    lv_textarea_set_max_length(s_chcreate_psk, 64);
    lv_textarea_set_placeholder_text(s_chcreate_psk, "(optional)  e.g. 1PG7OiApB1nwvP+rz05pAQ==");
    lv_obj_set_style_bg_color(s_chcreate_psk, lv_color_hex(TH_INPUT), 0);
    lv_obj_set_style_text_color(s_chcreate_psk, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(s_chcreate_psk, MCUI_FONT_16, 0);
    lv_obj_set_style_border_width(s_chcreate_psk, 0, 0);
    lv_obj_set_style_radius(s_chcreate_psk, 0, 0);
    lv_obj_set_style_anim_duration(s_chcreate_psk, 0, LV_PART_CURSOR);
    lv_obj_add_event_cb(s_chcreate_psk, chcreate_focus_cb, LV_EVENT_FOCUSED, nullptr);
    y += 50;

    s_chcreate_uplink_sw = chcreate_add_switch_row(card, "MQTT uplink", y, false);
    y += 50;
    s_chcreate_downlink_sw = chcreate_add_switch_row(card, "MQTT downlink", y, false);
    y += 50;

    s_chcreate_status = lv_label_create(card);
    lv_label_set_text(s_chcreate_status, "");
    lv_obj_set_width(s_chcreate_status, lv_pct(100));
    lv_label_set_long_mode(s_chcreate_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_chcreate_status, lv_color_hex(TH_TEXT3), 0);
    lv_obj_set_style_text_font(s_chcreate_status, MCUI_FONT_16, 0);
    lv_obj_align(s_chcreate_status, LV_ALIGN_TOP_LEFT, 0, y);

    lv_obj_t *cancel = lv_button_create(card);
    lv_obj_set_size(cancel, 130, 42);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(TH_INPUT), 0);
    lv_obj_set_style_radius(cancel, 0, 0);
    lv_obj_add_event_cb(cancel, chcreate_cancel_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *cl = lv_label_create(cancel);
    lv_label_set_text(cl, "Cancel");
    lv_obj_set_style_text_color(cl, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(cl, MCUI_FONT_16, 0);
    lv_obj_center(cl);

    lv_obj_t *ok = lv_button_create(card);
    lv_obj_set_size(ok, 130, 42);
    lv_obj_align(ok, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(ok, lv_color_hex(TH_ACCENT), 0);
    lv_obj_set_style_radius(ok, 0, 0);
    lv_obj_add_event_cb(ok, chcreate_ok_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *ol = lv_label_create(ok);
    lv_label_set_text(ol, "Create / join");
    lv_obj_set_style_text_color(ol, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(ol, MCUI_FONT_16, 0);
    lv_obj_center(ol);

    // Tap anywhere outside a text field (incl. the dimmed backdrop) to dismiss
    // the keyboard so the Cancel / Create buttons are always reachable.
    lv_obj_add_flag(s_chcreate_overlay, LV_OBJ_FLAG_CLICKABLE);
    chcreate_bind_dismiss_recursive(s_chcreate_overlay);

    keyboard_attach(s_chcreate_name);
    keyboard_show();
    lv_textarea_set_cursor_pos(s_chcreate_name, LV_TEXTAREA_CURSOR_LAST);
}

static void chats_fab_clicked_cb(lv_event_t *)
{

    bool has_room = false;
    for (uint8_t i = 1; i < channels.getNumChannels(); i++) {
        if (channels.getByIndex(i).role == meshtastic_Channel_Role_DISABLED) {
            has_room = true;
            break;
        }
    }
    if (!has_room) {

        LOG_WARN("mcui: chats FAB: no free secondary channel slot");
        return;
    }
    chcreate_open();
}

static void add_section_header(const char *text)
{
    lv_obj_t *h = lv_label_create(s_chats_list);
    lv_label_set_text(h, text);
    lv_obj_set_style_text_color(h, lv_color_hex(TH_TEXT3), 0);
    lv_obj_set_style_text_font(h, MCUI_FONT_16, 0);
    lv_obj_set_style_pad_top(h, 6, 0);
    lv_obj_set_style_pad_left(h, 12, 0);
}

static void add_chat_card(ChatEntry *ent, const char *subtitle, uint16_t unread,
                          uint32_t accent_color)
{
    lv_obj_t *card = lv_obj_create(s_chats_list);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_height(card, 64);
    lv_obj_set_style_bg_color(card, lv_color_hex(TH_SURFACE), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 0, 0);
    lv_obj_set_style_pad_all(card, 10, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(card, ent);
    lv_obj_add_event_cb(card, chat_card_clicked, LV_EVENT_CLICKED, nullptr);

    lv_obj_add_event_cb(card, chat_card_long_pressed, LV_EVENT_LONG_PRESSED, nullptr);

    lv_obj_t *dot = lv_obj_create(card);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 40, 40);
    lv_obj_set_pos(dot, 0, 2);
    lv_obj_set_style_radius(dot, 0, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(accent_color), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);

    char initial[6] = {0};
    if (ent->title[0]) {
        initial[0] = ent->title[0];
        initial[1] = '\0';
    } else {
        initial[0] = '?';
    }
    lv_obj_t *dotl = lv_label_create(dot);
    lv_label_set_text(dotl, initial);
    lv_obj_set_style_text_color(dotl, lv_color_hex(TH_TEXT), 0);
    lv_obj_center(dotl);

    lv_obj_t *tl = lv_label_create(card);
    lv_label_set_text(tl, ent->title);
    lv_obj_set_style_text_color(tl, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(tl, MCUI_FONT_16, 0);
    lv_obj_set_pos(tl, 52, 2);
    lv_obj_set_width(tl, SCR_W - 52 - 20 - 40);
    lv_label_set_long_mode(tl, LV_LABEL_LONG_DOT);

    if (subtitle && subtitle[0]) {
        lv_obj_t *sl = lv_label_create(card);
        lv_label_set_text(sl, subtitle);
        lv_obj_set_style_text_color(sl, lv_color_hex(TH_TEXT2), 0);
        lv_obj_set_style_text_font(sl, MCUI_FONT_16, 0);
        lv_obj_set_pos(sl, 52, 24);
        lv_obj_set_width(sl, SCR_W - 52 - 20 - 40);
        lv_label_set_long_mode(sl, LV_LABEL_LONG_DOT);
    }

    if (unread > 0) {
        lv_obj_t *badge = lv_obj_create(card);
        lv_obj_remove_style_all(badge);
        lv_obj_set_size(badge, 26, 22);
        lv_obj_set_style_radius(badge, 0, 0);
        lv_obj_set_style_bg_color(badge, lv_color_hex(TH_ACCENT), 0);
        lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
        lv_obj_align(badge, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_remove_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(badge, LV_OBJ_FLAG_CLICKABLE);

        char num[6];
        snprintf(num, sizeof(num), "%u", (unsigned)(unread > 99 ? 99 : unread));
        lv_obj_t *bl = lv_label_create(badge);
        lv_label_set_text(bl, num);
        lv_obj_set_style_text_color(bl, lv_color_hex(TH_TEXT), 0);
        lv_obj_center(bl);
    }
}

static void fill_preview(const McConvId &id, char *out, size_t out_sz)
{
    McMessage last;
    if (!messages_last(id, last)) {
        snprintf(out, out_sz, "No messages yet");
        return;
    }
    const char *prefix = last.outgoing ? "You: " : "";
    snprintf(out, out_sz, "%s%s", prefix, last.text);
}

struct ConvGather {
    McConvId ids[MC_MAX_CONVERSATIONS];
    int n;
};
static void conv_gather_cb(const McConvId &id, void *ctx)
{
    ConvGather *g = (ConvGather *)ctx;
    if (g->n < (int)(sizeof(g->ids) / sizeof(g->ids[0]))) {
        g->ids[g->n++] = id;
    }
}

static bool is_mqtt_only_contact(const meshtastic_NodeInfoLite *n)
{
#if defined(CROWPANEL_DHE04005D)
    return n && n->via_mqtt && n->snr == 0.0f && !moduleConfig.mqtt.mqtt_contact_discovery_enabled;
#else
    (void)n;
    return false;
#endif
}

static void rebuild_chats_list()
{
    if (!s_chats_list) return;
    lv_obj_clean(s_chats_list);
    s_num_entries = 0;

    add_section_header("Channels");

    uint8_t nch = channels.getNumChannels();
    for (uint8_t i = 0; i < nch && s_num_entries < (int)(sizeof(s_entries)/sizeof(s_entries[0])); i++) {
        meshtastic_Channel &ch = channels.getByIndex(i);
        if (ch.role == meshtastic_Channel_Role_DISABLED) continue;

        ChatEntry *ent = &s_entries[s_num_entries++];
        ent->id = McConvId::channel(i);
        const char *name = channels.getName(i);
        if (!name || !name[0]) name = (i == 0) ? "Primary" : "Channel";
        strncpy(ent->title, name, sizeof(ent->title) - 1);
        ent->title[sizeof(ent->title) - 1] = '\0';

        char preview[80];
        fill_preview(ent->id, preview, sizeof(preview));
        uint16_t u = messages_unread(ent->id);
        add_chat_card(ent, preview, u, avatar_color_for(ent->id, ent->title));
    }

    ConvGather g;
    g.n = 0;
    messages_for_each_conv(conv_gather_cb, &g);

    bool any_direct = false;
    for (int i = 0; i < g.n; i++) {
        if (g.ids[i].kind != McConvId::DIRECT) continue;
        meshtastic_NodeInfoLite *n = nodeDB ? nodeDB->getMeshNode((NodeNum)g.ids[i].value) : nullptr;
        if (is_mqtt_only_contact(n)) continue;
        any_direct = true;
        break;
    }
    if (any_direct) {
        add_section_header("Direct");
    }

    for (int i = 0; i < g.n && s_num_entries < (int)(sizeof(s_entries)/sizeof(s_entries[0])); i++) {
        if (g.ids[i].kind != McConvId::DIRECT) continue;
        meshtastic_NodeInfoLite *n = nodeDB ? nodeDB->getMeshNode((NodeNum)g.ids[i].value) : nullptr;
        if (is_mqtt_only_contact(n)) continue;

        ChatEntry *ent = &s_entries[s_num_entries++];
        ent->id = g.ids[i];

        const char *title = nullptr;
        char fallback[16];
        if (n && n->has_user) {
            if (n->user.long_name[0])
                title = n->user.long_name;
            else if (n->user.short_name[0])
                title = n->user.short_name;
        }
        if (!title) {
            snprintf(fallback, sizeof(fallback), "!%08x", (unsigned)g.ids[i].value);
            title = fallback;
        }
        strncpy(ent->title, title, sizeof(ent->title) - 1);
        ent->title[sizeof(ent->title) - 1] = '\0';

        char preview[80];
        fill_preview(ent->id, preview, sizeof(preview));
        uint16_t u = messages_unread(ent->id);
        add_chat_card(ent, preview, u, avatar_color_for(ent->id, ent->title));
    }

    s_chats_last_tick = messages_change_tick();
}

lv_obj_t *chats_screen_create(lv_obj_t *parent)
{
    s_chats_page = make_page(parent);

    s_chats_list = lv_obj_create(s_chats_page);
    lv_obj_remove_style_all(s_chats_list);
    lv_obj_set_size(s_chats_list, SCR_W, PAGE_H);
    lv_obj_set_pos(s_chats_list, 0, 0);
    lv_obj_set_style_bg_color(s_chats_list, lv_color_hex(TH_BG), 0);
    lv_obj_set_style_bg_opa(s_chats_list, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_chats_list, 8, 0);
    lv_obj_set_style_pad_row(s_chats_list, 6, 0);
    lv_obj_set_flex_flow(s_chats_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_chats_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_chats_list, LV_SCROLLBAR_MODE_AUTO);

    lv_obj_t *fab = lv_button_create(s_chats_page);
    lv_obj_set_size(fab, 56, 56);
    lv_obj_align(fab, LV_ALIGN_BOTTOM_RIGHT, -16, -16);
    lv_obj_set_style_bg_color(fab, lv_color_hex(TH_ACCENT), 0);
    lv_obj_set_style_radius(fab, 0, 0);
    lv_obj_set_style_shadow_width(fab, 0, 0);
    lv_obj_set_style_shadow_opa(fab, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(fab, chats_fab_clicked_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_move_foreground(fab);

    lv_obj_t *fab_lbl = lv_label_create(fab);
    lv_label_set_text(fab_lbl, LV_SYMBOL_PLUS);
    lv_obj_set_style_text_color(fab_lbl, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(fab_lbl, MCUI_FONT_16, 0);
    lv_obj_center(fab_lbl);

    s_chats_last_tick = 0xFFFFFFFFu;
    rebuild_chats_list();
    return s_chats_page;
}

void chats_screen_tick()
{

    chatview_tick();

    if (!s_chats_list) return;
    if (chatview_is_open()) return;
    uint32_t t = messages_change_tick();
    if (t != s_chats_last_tick) {
        rebuild_chats_list();
    }
}

struct MapTileSlot {
    lv_obj_t *img = nullptr;
    lv_obj_t *placeholder = nullptr;
    char src[96] = {0};
    // Keep resolved tile identity across layout changes, including missing files.
    int cz = -1, cx = -1, cy = -1;
    bool cloaded = false; // whether (cz,cx,cy) resolved to a real tile file
    MapTileKey target;
    bool active = false;
    bool pending = false;
    bool visible = false;
};

struct MapMarkerSlot {
    lv_obj_t *dot = nullptr;
    lv_obj_t *tag = nullptr;
    lv_obj_t *tag_label = nullptr;
    bool active = false;
    bool has_tag = false; // whether this slot's tag/label should be visible
    uint32_t node_num = 0;
    int count = 1;
};

struct MapState {
    lv_obj_t *page = nullptr;
    lv_obj_t *content = nullptr;
    lv_obj_t *pan = nullptr;
    lv_obj_t *status = nullptr;
    lv_obj_t *zoom_lbl = nullptr;
    double center_lat = 51.21075;
    double center_lon = 4.43384;
    int zoom = 11;
    int drag_dx = 0;
    int drag_dy = 0;
    uint32_t last_nodes_refresh_ms = 0;
    bool initialized = false;
    bool first_render_pending = true;
    bool show_names = false;
    bool save_pending = false;
    uint32_t save_changed_ms = 0;
    int base_x = 0, base_y = 0;
    int pins = 0;
    int positioned_nodes = 0;
    int fallback_start_zoom = -1;
    bool fallback_active = false;
    bool tiles_ready = false;
    int gesture_travel = 0;
};

static constexpr int kMapTilePx = 256;
static constexpr int kMapMarkerDotPx = 8;
static constexpr int kMapLocalDotPx = 10;
static constexpr int kMapMinZoom = 7;
static constexpr int kMapMaxZoom = 18;
static constexpr int kMapDesiredTilePad = 1;
// Must fit in LV_CACHE_DEF_SIZE at 256 KB per decoded tile (9.5 MB cache holds
// 30 with margin). The 2026-07 map-freeze was NOT a tile-count/cache issue —
// it was LVGL's builtin lv_tlsf allocator infinite-looping during image decode
// (fixed by the multi_heap custom allocator, crowpanel_lvgl_alloc.cpp).
static constexpr int kMapMaxTileSlots = 30;
static constexpr int kMapMaxMarkerSlots = 40;
// Screen-space grid cell (px) for marker clustering: nodes whose dots fall in
// the same cell collapse into one dot + a count, so a crowded area doesn't draw
// dozens of overlapping markers. Zoom in to pull them apart.
static constexpr int kMapClusterCellPx = 40;
static constexpr uint32_t kMapNodeRefreshMs = 2000;
static constexpr uint32_t kMapMarkerRestoreDelayMs = 120;
static constexpr int kMapRebuildEdgeMarginPx = 24;
static MapState s_map;
static MapTileSlot s_map_tiles[kMapMaxTileSlots];
static MapMarkerSlot s_map_markers[kMapMaxMarkerSlots];
static bool s_map_markers_hidden = false;
static bool s_map_blocked_by_wifi = false;
static uint32_t s_map_marker_restore_ms = 0;
static uint32_t s_map_last_diag_ms = 0;
static lv_obj_t *s_map_popup = nullptr;

#if defined(ARCH_ESP32) && defined(CROWPANEL_DHE04005D) && !defined(CROWPANEL_DISABLE_MAPS)
static sdmmc_card_t *s_map_sd_card = nullptr;
static bool s_map_sd_ready = false;
static bool s_map_sd_attempted = false;
static bool s_map_sd_own_mount = false;
static volatile uint8_t s_map_sd_mount_state = 0; // 0=idle, 1=ready, 2=failed
#endif

static double maps_world_size_px(int zoom)
{
    return 256.0 * (double)(1 << zoom);
}

static void maps_latlon_to_world(double lat, double lon, int zoom, double *wx, double *wy)
{
    // Mercator is undefined at the poles, which fixed/stale positions can contain.
    lat = fmax(-85.05112878, fmin(85.05112878, lat));
    const double n = maps_world_size_px(zoom);
    *wx = (lon + 180.0) / 360.0 * n;
    const double lat_rad = lat * M_PI / 180.0;
    *wy = (1.0 - asinh(tan(lat_rad)) / M_PI) / 2.0 * n;
}

static void maps_world_to_latlon(double wx, double wy, int zoom, double *lat, double *lon)
{
    const double n = maps_world_size_px(zoom);
    *lon = wx / n * 360.0 - 180.0;
    const double y_norm = wy / n;
    const double lat_rad = atan(sinh(M_PI * (1.0 - 2.0 * y_norm)));
    *lat = lat_rad * 180.0 / M_PI;
}

static void maps_status_set(const char *text)
{
    if (s_map.status) lv_label_set_text(s_map.status, text ? text : "");
}

static bool maps_wifi_conflicts_with_sd()
{
#if defined(ARCH_ESP32P4) && defined(CROWPANEL_DHE04005D)
#if !defined(CROWPANEL_DISABLE_MAPS)
    if (s_map_sd_ready) return false;
#endif
    struct stat st = {};
    if (stat("/sdcard", &st) == 0 && (st.st_mode & S_IFDIR))
        return false;
    return config.network.wifi_enabled && config.network.wifi_ssid[0] != '\0';
#else
    return false;
#endif
}

static void maps_zoom_set_label()
{
    if (!s_map.zoom_lbl) return;
    char buf[24];
    snprintf(buf, sizeof(buf), "Z%d", s_map.zoom);
    lv_label_set_text(s_map.zoom_lbl, buf);
}

static void maps_viewport_save_now()
{
    Preferences prefs;
    if (!prefs.begin("map", false)) {
        s_map.save_changed_ms = millis();
        return;
    }
    prefs.putInt("lat_e6", (int32_t)lround(s_map.center_lat * 1.0e6));
    prefs.putInt("lon_e6", (int32_t)lround(s_map.center_lon * 1.0e6));
    prefs.putUChar("zoom", (uint8_t)s_map.zoom);
    prefs.putBool("names", s_map.show_names);
    prefs.end();
    s_map.save_pending = false;
}

static void maps_viewport_save_later()
{
    s_map.save_pending = true;
    s_map.save_changed_ms = millis();
}

static bool maps_viewport_load()
{
    Preferences prefs;
    if (!prefs.begin("map", true))
        return false;
    const int32_t lat_e6 = prefs.getInt("lat_e6", INT32_MIN);
    const int32_t lon_e6 = prefs.getInt("lon_e6", INT32_MIN);
    uint8_t zoom = prefs.getUChar("zoom", 0xFF);
    s_map.show_names = prefs.getBool("names", false);
    prefs.end();

    if (lat_e6 == INT32_MIN || lon_e6 == INT32_MIN || zoom == 0xFF)
        return false;
    if (lat_e6 < -85051129 || lat_e6 > 85051129 || lon_e6 < -180000000 || lon_e6 > 180000000)
        return false;
    if (zoom < kMapMinZoom)
        zoom = kMapMinZoom;
    if (zoom > kMapMaxZoom)
        zoom = kMapMaxZoom;

    s_map.center_lat = (double)lat_e6 / 1.0e6;
    s_map.center_lon = (double)lon_e6 / 1.0e6;
    s_map.zoom = (int)zoom;
    return true;
}

static bool maps_center_from_local_node()
{
    if (!nodeDB) return false;
    meshtastic_NodeInfoLite *local = nodeDB->getMeshNode(nodeDB->getNodeNum());
    if (!local || !nodeDB->hasValidPosition(local)) return false;

    s_map.center_lat = (double)local->position.latitude_i / 1.0e7;
    s_map.center_lon = (double)local->position.longitude_i / 1.0e7;
    return true;
}

static bool maps_extract_node_latlon(const meshtastic_NodeInfoLite *node, double *lat, double *lon)
{
    if (!node || !lat || !lon) return false;

    // Keep drawing saved/fixed coordinates, but never project an invalid report.
    const int32_t lat_i = node->position.latitude_i;
    const int32_t lon_i = node->position.longitude_i;
    if (!PositionCoordinates::isValid(lat_i, lon_i)) return false;

    const double lat_d = (double)lat_i / 1.0e7;
    const double lon_d = (double)lon_i / 1.0e7;
    *lat = lat_d;
    *lon = lon_d;
    return true;
}

#if defined(CROWPANEL_DISABLE_MAPS)
void maps_storage_prewarm() {}
static bool maps_ensure_sd_mounted()
{
    return false;
}
#elif defined(ARCH_ESP32) && defined(CROWPANEL_DHE04005D)
void maps_storage_prewarm()
{
    if (s_map_sd_ready || s_map_sd_attempted) return;

    struct stat st = {};
    if (stat("/sdcard", &st) == 0 && (st.st_mode & S_IFDIR)) {
        s_map_sd_ready = true;
        s_map_sd_own_mount = false;
        s_map_sd_mount_state = 1;
        LOG_INFO("mcui: maps SD already mounted");
        return;
    }

    s_map_sd_attempted = true;
    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {};
    mount_cfg.format_if_mount_failed = false;
    mount_cfg.max_files = 6;
    mount_cfg.allocation_unit_size = 16 * 1024;

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = 10000;

    sdmmc_slot_config_t slot_cfg = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_cfg.clk = (gpio_num_t)SD_GPIO_MMC_CLK;
    slot_cfg.cmd = (gpio_num_t)SD_GPIO_MMC_CMD;
    slot_cfg.d0 = (gpio_num_t)SD_GPIO_MMC_D0;
    slot_cfg.width = 1;
    slot_cfg.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_err_t err = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot_cfg, &mount_cfg, &s_map_sd_card);
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        s_map_sd_ready = true;
        s_map_sd_own_mount = (err == ESP_OK);
        s_map_sd_mount_state = 1;
        LOG_INFO("mcui: maps SD mount ready");
    } else {
        s_map_sd_mount_state = 2;
        LOG_WARN("mcui: maps SD mount failed: %s", esp_err_to_name(err));
    }
}

static bool maps_ensure_sd_mounted()
{
    if (s_map_sd_ready) return true;

    struct stat st = {};
    if (stat("/sdcard", &st) == 0 && (st.st_mode & S_IFDIR)) {
        s_map_sd_ready = true;
        s_map_sd_own_mount = false;
        s_map_sd_mount_state = 1;
        return true;
    }
    maps_storage_prewarm();
    if (s_map_sd_ready) return true;
    return false;
}
#else
void maps_storage_prewarm() {}
static bool maps_ensure_sd_mounted()
{
    return false;
}
#endif

// Locate tile z/x/y on the SD card and point the slot's image at it.
// Prefers a .png tile, falling back to a raw LVGL .bin tile. The SD card is
// mounted at "/sdcard" and LVGL's POSIX driver maps drive "S:" onto that same
// path (LV_FS_POSIX_PATH), so the on-disk check uses "/sdcard/..." while LVGL
// is handed the equivalent "S:/..." path. Returns true if the tile is loaded.
static bool maps_load_tile(MapTileSlot &ts, int z, int x, int y)
{
    // Already-resolved tiles need no SD lookup, even after shifting the viewport.
    if (ts.cz == z && ts.cx == x && ts.cy == y)
        return ts.cloaded;

    static const char *const kExts[] = {"png", "bin"};

    char diskPath[64];
    struct stat st;
    const char *ext = nullptr;
    for (const char *e : kExts) {
        snprintf(diskPath, sizeof(diskPath), "/sdcard/tiles/%d/%d/%d.%s", z, x, y, e);
        if (stat(diskPath, &st) == 0 && st.st_size > 0) {
            ext = e;
            break;
        }
    }

    ts.cz = z;
    ts.cx = x;
    ts.cy = y;

    if (!ext) {
        ts.cloaded = false;
        return false;
    }

    char fsPath[64];
    snprintf(fsPath, sizeof(fsPath), "S:/tiles/%d/%d/%d.%s", z, x, y, ext);

    // Re-point the image only when the source actually changes; otherwise reuse
    // the already-decoded (LVGL image-cache) tile. stat() above already proved
    // the file exists, and set_src + the decode-on-render path handle a bad file
    // gracefully, so the extra lv_image_decoder_get_info() open was dropped to
    // halve SD reads per newly-exposed tile on pan/zoom.
    if (strcmp(ts.src, fsPath) != 0) {
        lv_image_set_src(ts.img, nullptr);
        strncpy(ts.src, fsPath, sizeof(ts.src) - 1);
        ts.src[sizeof(ts.src) - 1] = '\0';
        lv_image_set_src(ts.img, ts.src);
        lv_obj_set_size(ts.img, kMapTilePx, kMapTilePx);
        // Tiles are already native kMapTilePx (256) square, so no scaling is
        // needed. STRETCH forced LVGL's transformed/scaled ARGB8888 draw path
        // (slow, per-pixel alpha, and prone to stalling the SW renderer) even
        // at a 1:1 ratio; TOP_LEFT keeps the plain, fast, non-transformed blit.
        lv_image_set_inner_align(ts.img, LV_IMAGE_ALIGN_TOP_LEFT);
    }
    ts.cloaded = true;
    return true;
}

static void maps_clear_all_tiles()
{
    for (int i = 0; i < kMapMaxTileSlots; i++) {
        MapTileSlot &ts = s_map_tiles[i];
        ts.src[0] = '\0';
        ts.cz = ts.cx = ts.cy = -1; // invalidate the SD-skip cache
        ts.cloaded = false;
        ts.active = ts.pending = false;
        if (ts.img) {
            lv_image_set_src(ts.img, nullptr);
            lv_obj_add_flag(ts.img, LV_OBJ_FLAG_HIDDEN);
        }
        if (ts.placeholder)
            lv_obj_add_flag(ts.placeholder, LV_OBJ_FLAG_HIDDEN);
    }
}

static void maps_storage_release()
{
#if defined(ARCH_ESP32) && defined(CROWPANEL_DHE04005D) && !defined(CROWPANEL_DISABLE_MAPS)
    if (s_map_sd_own_mount && s_map_sd_card) {
        esp_vfs_fat_sdcard_unmount("/sdcard", s_map_sd_card);
        LOG_INFO("mcui: maps SD unmounted");
    }
    s_map_sd_card = nullptr;
    s_map_sd_ready = false;
    s_map_sd_attempted = false;
    s_map_sd_own_mount = false;
    s_map_sd_mount_state = 0;
#endif
}

static void maps_hide_all_markers()
{
    for (int i = 0; i < kMapMaxMarkerSlots; i++) {
        s_map_markers[i].active = false;
        s_map_markers[i].has_tag = false;
        if (s_map_markers[i].dot) lv_obj_add_flag(s_map_markers[i].dot, LV_OBJ_FLAG_HIDDEN);
        if (s_map_markers[i].tag) lv_obj_add_flag(s_map_markers[i].tag, LV_OBJ_FLAG_HIDDEN);
    }
}

static void maps_set_marker_visibility(bool visible)
{
    for (int i = 0; i < kMapMaxMarkerSlots; i++) {
        MapMarkerSlot &m = s_map_markers[i];
        if (!m.active) continue;
        // Pins travel with the tile layer; only labels disappear during drag.
        if (m.tag) {
            if (visible && m.has_tag)
                lv_obj_clear_flag(m.tag, LV_OBJ_FLAG_HIDDEN);
            else
                lv_obj_add_flag(m.tag, LV_OBJ_FLAG_HIDDEN);
        }
    }
    s_map_markers_hidden = !visible;
}

static void maps_node_short_name(const meshtastic_NodeInfoLite *node, char *out, size_t out_sz)
{
    if (!out || out_sz == 0) return;
    out[0] = '\0';
    if (!node) return;

    if (node->has_user && node->user.short_name[0]) {
        strncpy(out, node->user.short_name, out_sz - 1);
        out[out_sz - 1] = '\0';
        return;
    }

    if (node->has_user && node->user.long_name[0]) {
        strncpy(out, node->user.long_name, out_sz - 1);
        out[out_sz - 1] = '\0';
        return;
    }

    snprintf(out, out_sz, "!%08x", (unsigned)node->num);
}

static bool maps_slot_position_from_node(const meshtastic_NodeInfoLite *node, int base_x, int base_y, int pan_w, int pan_h, int *mx, int *my)
{
    if (!node || !mx || !my) return false;
    double lat = 0.0, lon = 0.0;
    if (!maps_extract_node_latlon(node, &lat, &lon)) return false;

    double wx = 0.0, wy = 0.0;
    maps_latlon_to_world(lat, lon, s_map.zoom, &wx, &wy);
    *mx = (int)lround(wx - base_x);
    *my = (int)lround(wy - base_y);

    return !(*mx < -8 || *my < -8 || *mx > pan_w + 8 || *my > pan_h + 8);
}

static void maps_show_marker_slot(MapMarkerSlot &slot, const meshtastic_NodeInfoLite *node, bool is_local, int mx, int my,
                                  bool show_name)
{
    if (!slot.dot) return;
    slot.node_num = node ? node->num : 0;
    slot.count = 1;

    if (is_local) {
        lv_obj_set_size(slot.dot, kMapLocalDotPx, kMapLocalDotPx);
        lv_obj_set_pos(slot.dot, mx - (kMapLocalDotPx / 2), my - (kMapLocalDotPx / 2));
        lv_obj_set_style_bg_color(slot.dot, lv_color_hex(0x6DC264), 0);
        lv_obj_set_style_radius(slot.dot, LV_RADIUS_CIRCLE, 0);
    } else {
        lv_obj_set_size(slot.dot, kMapMarkerDotPx, kMapMarkerDotPx);
        lv_obj_set_pos(slot.dot, mx - (kMapMarkerDotPx / 2), my - (kMapMarkerDotPx / 2));
        const bool router = node && node->has_user &&
            (node->user.role == meshtastic_Config_DeviceConfig_Role_ROUTER ||
             node->user.role == meshtastic_Config_DeviceConfig_Role_REPEATER ||
             node->user.role == meshtastic_Config_DeviceConfig_Role_ROUTER_LATE ||
             node->user.role == meshtastic_Config_DeviceConfig_Role_CLIENT_BASE);
        lv_obj_set_style_bg_color(slot.dot, lv_color_hex(router ? 0xFF3B30 : 0x35A4FF), 0);
        lv_obj_set_style_radius(slot.dot, LV_RADIUS_CIRCLE, 0);
    }

    lv_obj_move_foreground(slot.dot);
    lv_obj_clear_flag(slot.dot, LV_OBJ_FLAG_HIDDEN);

    // Optional MeshCore-style name tag, bounded to the visible viewport.
    if (show_name && slot.tag && slot.tag_label && node) {
        char short_name[64];
        maps_node_short_name(node, short_name, sizeof(short_name));
        if (strcmp(lv_label_get_text(slot.tag_label), short_name) != 0)
            lv_label_set_text(slot.tag_label, short_name);
        lv_obj_set_size(slot.tag, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_pos(slot.tag, mx + 7, my - 10);
        lv_obj_move_foreground(slot.tag);
        lv_obj_clear_flag(slot.tag, LV_OBJ_FLAG_HIDDEN);
        slot.has_tag = true;
    } else if (slot.tag) {
        lv_obj_add_flag(slot.tag, LV_OBJ_FLAG_HIDDEN);
        slot.has_tag = false;
    }

    slot.active = true;
}

// A cluster marker: one dot standing in for `count` nearby nodes, with the
// count shown in the same tag styling as a name. The count is drawn at every
// zoom (it's the whole point of collapsing the group).
static void maps_show_cluster_slot(MapMarkerSlot &slot, int count, int mx, int my)
{
    if (!slot.dot) return;
    slot.node_num = 0;
    slot.count = count;

    lv_obj_set_size(slot.dot, kMapMarkerDotPx, kMapMarkerDotPx);
    lv_obj_set_pos(slot.dot, mx - (kMapMarkerDotPx / 2), my - (kMapMarkerDotPx / 2));
    lv_obj_set_style_bg_color(slot.dot, lv_color_hex(0x35A4FF), 0);
    lv_obj_set_style_radius(slot.dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_move_foreground(slot.dot);
    lv_obj_clear_flag(slot.dot, LV_OBJ_FLAG_HIDDEN);

    if (slot.tag && slot.tag_label) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", count);
        if (strcmp(lv_label_get_text(slot.tag_label), buf) != 0)
            lv_label_set_text(slot.tag_label, buf);
        lv_obj_set_size(slot.tag, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_pos(slot.tag, mx + 6, my - 10);
        lv_obj_move_foreground(slot.tag);
        lv_obj_clear_flag(slot.tag, LV_OBJ_FLAG_HIDDEN);
        slot.has_tag = true;
    }

    slot.active = true;
}

static int maps_draw_markers(int base_x, int base_y, int pan_w, int pan_h)
{
    maps_hide_all_markers();
    s_map.positioned_nodes = 0;
    if (!nodeDB || !s_map.pan) return 0;

    const uint32_t local_num = nodeDB->getNodeNum();
    const bool show_names = s_map.show_names;

    // Bin visible non-local nodes into screen-space grid cells. Nodes sharing a
    // cell collapse to one marker: a single node keeps its dot (+ name at close
    // zoom), several become one dot + a count. Deterministic (cell id = integer
    // grid coords), and O(nodes * clusters) which is fine for a few hundred
    // nodes. Static (single UI task) to keep it off the stack.
    struct MapCluster {
        int cx, cy;                          // grid cell
        int count;                           // nodes in this cell
        int sum_mx, sum_my;                  // for centroid placement
        meshtastic_NodeInfoLite *node;       // representative (used when count == 1)
    };
    static MapCluster clusters[kMapMaxMarkerSlots];
    int nclusters = 0;

    const size_t n = nodeDB->getNumMeshNodes();
    for (size_t i = 0; i < n; i++) {
        meshtastic_NodeInfoLite *node = nodeDB->getMeshNodeByIndex(i);
        if (!node || node->num == local_num) continue; // local drawn separately
        if (PositionCoordinates::isValid(node->position.latitude_i, node->position.longitude_i))
            ++s_map.positioned_nodes;
        int mx = 0, my = 0;
        if (!maps_slot_position_from_node(node, base_x, base_y, pan_w, pan_h, &mx, &my)) continue;

        const int cx = (int)floor((double)mx / kMapClusterCellPx);
        const int cy = (int)floor((double)my / kMapClusterCellPx);
        int found = -1;
        for (int c = 0; c < nclusters; c++) {
            if (clusters[c].cx == cx && clusters[c].cy == cy) { found = c; break; }
        }
        if (found < 0) {
            if (nclusters >= kMapMaxMarkerSlots - 1) continue; // reserve the local pin
            found = nclusters++;
            clusters[found] = {cx, cy, 0, 0, 0, node};
        }
        clusters[found].count++;
        clusters[found].sum_mx += mx;
        clusters[found].sum_my += my;
    }

    int marker_idx = 0;
    for (int c = 0; c < nclusters && marker_idx < kMapMaxMarkerSlots; c++) {
        MapCluster &cl = clusters[c];
        const int cmx = cl.sum_mx / cl.count;
        const int cmy = cl.sum_my / cl.count;
        if (cl.count == 1)
            maps_show_marker_slot(s_map_markers[marker_idx], cl.node, false, cmx, cmy, show_names);
        else
            maps_show_cluster_slot(s_map_markers[marker_idx], cl.count, cmx, cmy);
        marker_idx++;
    }

    // Local node: always its own marker (distinct style), never clustered.
    if (marker_idx < kMapMaxMarkerSlots) {
        meshtastic_NodeInfoLite *local = nodeDB->getMeshNode(local_num);
        if (local && PositionCoordinates::isValid(local->position.latitude_i, local->position.longitude_i))
            ++s_map.positioned_nodes;
        int mx = 0, my = 0;
        if (maps_slot_position_from_node(local, base_x, base_y, pan_w, pan_h, &mx, &my)) {
            maps_show_marker_slot(s_map_markers[marker_idx], local, true, mx, my, show_names);
            marker_idx++;
        }
    }
    return marker_idx;
}

static bool maps_should_rebuild_after_drag()
{
    if (!s_map.content || !s_map.pan) return true;
    const int view_w = lv_obj_get_width(s_map.content);
    const int view_h = lv_obj_get_height(s_map.content);
    const int x = lv_obj_get_x(s_map.pan);
    const int y = lv_obj_get_y(s_map.pan);
    const int w = lv_obj_get_width(s_map.pan);
    const int h = lv_obj_get_height(s_map.pan);
    if (w <= 0 || h <= 0) return true;

    const bool cover_x = (x <= kMapRebuildEdgeMarginPx) && (x + w >= view_w - kMapRebuildEdgeMarginPx);
    const bool cover_y = (y <= kMapRebuildEdgeMarginPx) && (y + h >= view_h - kMapRebuildEdgeMarginPx);
    return !(cover_x && cover_y);
}

static void maps_schedule_marker_restore()
{
    s_map_marker_restore_ms = millis();
}

static void maps_refresh_markers()
{
    s_map.pins = maps_draw_markers(s_map.base_x, s_map.base_y,
                                 lv_obj_get_width(s_map.pan), lv_obj_get_height(s_map.pan));
    // Limit labels and reject overlaps; pins remain visible in crowded areas.
    int shown = 0;
    const int left = -lv_obj_get_x(s_map.pan) + 4;
    const int top = -lv_obj_get_y(s_map.pan) + 40;
    const int right = left + lv_obj_get_width(s_map.content) - 8;
    const int bottom = top + lv_obj_get_height(s_map.content) - 44;
    lv_obj_update_layout(s_map.pan);
    for (int i = 0; i < kMapMaxMarkerSlots; ++i) {
        MapMarkerSlot &m = s_map_markers[i];
        if (!m.active || !m.has_tag) continue;
        const int w = lv_obj_get_width(m.tag), h = lv_obj_get_height(m.tag);
        const int x = static_cast<int>(fmax(left, fmin(right - w, lv_obj_get_x(m.tag))));
        const int y = static_cast<int>(fmax(top, fmin(bottom - h, lv_obj_get_y(m.tag))));
        lv_obj_set_pos(m.tag, x, y);
        const int px = lv_obj_get_x(m.dot), py = lv_obj_get_y(m.dot);
        bool hide = shown >= 22 || px < left || px > right || py < top || py > bottom;
        for (int j = 0; !hide && j < i; ++j) {
            const MapMarkerSlot &other = s_map_markers[j];
            if (!other.active || !other.has_tag) continue;
            const int ox = lv_obj_get_x(other.tag), oy = lv_obj_get_y(other.tag);
            hide = x < ox + lv_obj_get_width(other.tag) + 3 && x + w + 3 > ox &&
                   y < oy + lv_obj_get_height(other.tag) + 3 && y + h + 3 > oy;
        }
        if (hide) {
            m.has_tag = false;
            lv_obj_add_flag(m.tag, LV_OBJ_FLAG_HIDDEN);
        } else {
            ++shown;
        }
    }
    for (MapMarkerSlot &m : s_map_markers)
        if (m.active) lv_obj_move_foreground(m.dot);
    s_map_markers_hidden = false;
}

static int maps_tile_status()
{
    int loaded = 0, total = 0, pending = 0;
    for (const MapTileSlot &ts : s_map_tiles) {
        if (!ts.active) continue;
        const int limit = 1 << s_map.zoom;
        if (ts.target.x < 0 || ts.target.y < 0 || ts.target.x >= limit || ts.target.y >= limit) continue;
        ++total;
        if (ts.pending) ++pending;
        else if (ts.cloaded) ++loaded;
    }
    char status[88];
    snprintf(status, sizeof(status), "%d/%d tiles  %d located  %d pins%s", loaded, total, s_map.positioned_nodes, s_map.pins,
             pending ? "  " LV_SYMBOL_REFRESH : "");
    maps_status_set(status);
    return loaded;
}

static int maps_rebuild(bool allow_zoom_fallback, bool include_markers = true)
{
    if (!s_map.pan || !s_map.content) return 0;
    s_map.tiles_ready = false;
    if (allow_zoom_fallback) {
        s_map.fallback_start_zoom = s_map.zoom;
        s_map.fallback_active = true;
    } else {
        s_map.fallback_active = false;
    }

    if (maps_wifi_conflicts_with_sd()) {
        s_map_blocked_by_wifi = true;
        maps_status_set("Maps require WiFi off");
        maps_clear_all_tiles();
        maps_hide_all_markers();
        s_map.fallback_active = false;
        return 0;
    }
    s_map_blocked_by_wifi = false;

    if (!maps_ensure_sd_mounted()) {
#if defined(CROWPANEL_DISABLE_MAPS)
        maps_status_set("Maps disabled");
#elif defined(ARCH_ESP32) && defined(CROWPANEL_DHE04005D)
        if (s_map_sd_mount_state == 2)
            maps_status_set("SD mount failed");
        else
            maps_status_set("SD card not mounted");
#else
        maps_status_set("SD card not mounted");
#endif
        for (int i = 0; i < kMapMaxTileSlots; i++) {
            if (s_map_tiles[i].img) lv_obj_add_flag(s_map_tiles[i].img, LV_OBJ_FLAG_HIDDEN);
            if (s_map_tiles[i].placeholder) lv_obj_add_flag(s_map_tiles[i].placeholder, LV_OBJ_FLAG_HIDDEN);
        }
        maps_hide_all_markers();
        s_map.fallback_active = false;
        return 0;
    }

    const int view_w = lv_obj_get_width(s_map.content);
    const int view_h = lv_obj_get_height(s_map.content);
    double cwx = 0.0, cwy = 0.0;
    maps_latlon_to_world(s_map.center_lat, s_map.center_lon, s_map.zoom, &cwx, &cwy);

    const double origin_x = cwx - view_w / 2.0;
    const double origin_y = cwy - view_h / 2.0;
    const int vis_x0 = (int)floor(origin_x / kMapTilePx);
    const int vis_y0 = (int)floor(origin_y / kMapTilePx);
    const int vis_x1 = (int)floor((origin_x + view_w - 1) / kMapTilePx);
    const int vis_y1 = (int)floor((origin_y + view_h - 1) / kMapTilePx);
    const int vis_cols = vis_x1 - vis_x0 + 1;
    const int vis_rows = vis_y1 - vis_y0 + 1;

    int tile_pad = kMapDesiredTilePad;
    while (tile_pad > 0) {
        const int cols_try = vis_cols + 2 * tile_pad;
        const int rows_try = vis_rows + 2 * tile_pad;
        if (cols_try * rows_try <= kMapMaxTileSlots)
            break;
        tile_pad--;
    }

    const int x0 = vis_x0 - tile_pad;
    const int y0 = vis_y0 - tile_pad;
    const int x1 = vis_x1 + tile_pad;
    const int y1 = vis_y1 + tile_pad;

    const int cols = x1 - x0 + 1;
    const int rows = y1 - y0 + 1;
    const int pan_w = cols * kMapTilePx;
    const int pan_h = rows * kMapTilePx;
    const int pan_x = (int)lround(x0 * kMapTilePx - origin_x) + s_map.drag_dx;
    const int pan_y = (int)lround(y0 * kMapTilePx - origin_y) + s_map.drag_dy;

    lv_obj_set_size(s_map.pan, pan_w, pan_h);
    lv_obj_set_pos(s_map.pan, pan_x, pan_y);

    MapTileKey existing[kMapMaxTileSlots];
    int slots[kMapMaxTileSlots];
    for (int i = 0; i < kMapMaxTileSlots; ++i) {
        existing[i] = {s_map_tiles[i].cz, s_map_tiles[i].cx, s_map_tiles[i].cy};
        s_map_tiles[i].active = s_map_tiles[i].pending = false;
    }
    if (!map_tile_layout(existing, s_map.zoom, x0, y0, cols, rows, slots)) return 0;
    s_map.tiles_ready = true;
    int reused = 0, queued = 0;
    const int tile_max = 1 << s_map.zoom;
    for (int i = 0; i < cols * rows; ++i) {
        MapTileSlot &ts = s_map_tiles[slots[i]];
        const int tx = x0 + i % cols, ty = y0 + i / cols;
        ts.target = {s_map.zoom, tx, ty};
        ts.active = true;
        ts.visible = tx >= vis_x0 && tx <= vis_x1 && ty >= vis_y0 && ty <= vis_y1;
        lv_obj_set_pos(ts.placeholder, (i % cols) * kMapTilePx, (i / cols) * kMapTilePx);
        lv_obj_set_pos(ts.img, (i % cols) * kMapTilePx, (i / cols) * kMapTilePx);
        const bool valid = tx >= 0 && ty >= 0 && tx < tile_max && ty < tile_max;
        const bool resolved = existing[slots[i]] == ts.target;
        ts.pending = valid && !resolved;
        if (ts.pending) ++queued;
        if (resolved && ts.cloaded) ++reused;
        if (valid && resolved && ts.cloaded) {
            lv_obj_clear_flag(ts.img, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(ts.placeholder, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(ts.img, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(ts.placeholder, LV_OBJ_FLAG_HIDDEN);
        }
    }
    for (MapTileSlot &ts : s_map_tiles) {
        if (ts.active) continue;
        lv_obj_add_flag(ts.img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ts.placeholder, LV_OBJ_FLAG_HIDDEN);
    }
    s_map.base_x = x0 * kMapTilePx;
    s_map.base_y = y0 * kMapTilePx;

    if (include_markers) {
        maps_refresh_markers();
        s_map_marker_restore_ms = 0;
    } else {
        maps_hide_all_markers();
        s_map_markers_hidden = true;
    }

    maps_tile_status();
    LOG_DEBUG("mcui: map layout reused=%d queued=%d zoom=%d", reused, queued, s_map.zoom);
    return reused + queued;
}

static void maps_load_next_tile()
{
    if (!s_map.tiles_ready) return;
    // One tile per UI tick, visible area before the off-screen pan border.
    // All LVGL/decoder work stays under the existing UI lock and clean arena.
    MapTileSlot *next = nullptr;
    for (MapTileSlot &ts : s_map_tiles) {
        if (!ts.active || !ts.pending) continue;
        if (!next || (ts.visible && !next->visible)) next = &ts;
    }
    if (next) {
        const uint32_t start = millis();
        const bool loaded = maps_load_tile(*next, next->target.zoom, next->target.x, next->target.y);
        next->pending = false;
        if (loaded) {
            lv_obj_clear_flag(next->img, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(next->placeholder, LV_OBJ_FLAG_HIDDEN);
        }
        maps_tile_status();
        if (millis() - start > 150 && !Throttle::isWithinTimespanMs(s_map_last_diag_ms, 5000)) {
            s_map_last_diag_ms = millis();
            LOG_WARN("mcui: map tile load slow %u ms z=%d x=%d y=%d", (unsigned)(millis() - start),
                     next->target.zoom, next->target.x, next->target.y);
        }
        return;
    }
    if (!s_map.fallback_active) return;
    if (maps_tile_status() > 0) {
        s_map.fallback_active = false;
        if (s_map.zoom != s_map.fallback_start_zoom) maps_viewport_save_later();
        return;
    }
    const int original = s_map.fallback_start_zoom;
    if (s_map.zoom > kMapMinZoom) {
        --s_map.zoom;
        maps_zoom_set_label();
        maps_rebuild(false, true);
        s_map.fallback_start_zoom = original;
        s_map.fallback_active = true;
    } else {
        s_map.zoom = original;
        maps_zoom_set_label();
        maps_rebuild(false, true);
        maps_status_set("No tiles for this area");
        maps_viewport_save_later();
    }
}

static void maps_apply_drag_and_rebuild()
{
    if (s_map.drag_dx == 0 && s_map.drag_dy == 0) return;

    double cwx = 0.0, cwy = 0.0;
    maps_latlon_to_world(s_map.center_lat, s_map.center_lon, s_map.zoom, &cwx, &cwy);
    cwx -= s_map.drag_dx;
    cwy -= s_map.drag_dy;

    const double ws = maps_world_size_px(s_map.zoom);
    if (cwx < 0) cwx = 0;
    if (cwx > ws - 1) cwx = ws - 1;
    if (cwy < 0) cwy = 0;
    if (cwy > ws - 1) cwy = ws - 1;
    maps_world_to_latlon(cwx, cwy, s_map.zoom, &s_map.center_lat, &s_map.center_lon);

    s_map.drag_dx = 0;
    s_map.drag_dy = 0;

    if (maps_should_rebuild_after_drag()) {
        maps_rebuild(false, true);
        maps_schedule_marker_restore();
    } else {
        maps_schedule_marker_restore();
    }
    maps_viewport_save_later();
}

static void maps_on_pressing(lv_event_t *)
{
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev || !s_map.pan) return;
    lv_point_t v = {0, 0};
    lv_indev_get_vect(indev, &v);
    if (v.x == 0 && v.y == 0) return;

    s_map.gesture_travel += abs(v.x) + abs(v.y);
    s_map.fallback_active = false;
    if (s_map.gesture_travel > 10 && !s_map_markers_hidden)
        maps_set_marker_visibility(false);
    s_map_marker_restore_ms = 0;
    s_map.drag_dx += v.x;
    s_map.drag_dy += v.y;
    lv_obj_set_pos(s_map.pan, lv_obj_get_x(s_map.pan) + v.x, lv_obj_get_y(s_map.pan) + v.y);
}

static void maps_popup_close(lv_event_t * = nullptr)
{
    if (!s_map_popup) return;
    lv_obj_delete(s_map_popup);
    s_map_popup = nullptr;
}

static void maps_on_released(lv_event_t *)
{
    const bool tapped = s_map.gesture_travel <= 10;
    maps_apply_drag_and_rebuild();
    s_map.gesture_travel = 0;
    if (!tapped || !s_map.tiles_ready || !nodeDB) return;
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t point;
    lv_indev_get_point(indev, &point);
    MapMarkerSlot *nearest = nullptr;
    int best = 20 * 20 + 1;
    for (MapMarkerSlot &m : s_map_markers) {
        if (!m.active) continue;
        lv_area_t area;
        lv_obj_get_coords(m.dot, &area);
        const int dx = point.x - (area.x1 + area.x2) / 2;
        const int dy = point.y - (area.y1 + area.y2) / 2;
        const int distance = dx * dx + dy * dy;
        if (distance < best) { best = distance; nearest = &m; }
    }
    if (!nearest) return;
    char title[64], details[128];
    if (nearest->count > 1) {
        snprintf(title, sizeof(title), "%d nearby nodes", nearest->count);
        snprintf(details, sizeof(details), "Zoom %d", s_map.zoom);
    } else {
        const auto *node = nodeDB->getMeshNode(nearest->node_num);
        if (!node) return;
        maps_node_short_name(node, title, sizeof(title));
        double lat, lon;
        if (!maps_extract_node_latlon(node, &lat, &lon)) return;
        snprintf(details, sizeof(details), "!%08x\n%.5f, %.5f", (unsigned)nearest->node_num, lat, lon);
    }
    maps_popup_close();
    s_map_popup = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_map_popup);
    lv_obj_set_size(s_map_popup, SCR_W, SCR_H);
    lv_obj_set_style_bg_color(s_map_popup, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_map_popup, LV_OPA_60, 0);
    lv_obj_remove_flag(s_map_popup, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_map_popup, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_map_popup, maps_popup_close, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *box = lv_obj_create(s_map_popup);
    lv_obj_set_size(box, SCR_W - 40, 200);
    lv_obj_set_style_bg_color(box, lv_color_hex(TH_SURFACE), 0);
    lv_obj_set_style_radius(box, 6, 0);
    lv_obj_set_style_pad_all(box, 12, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(box);
    lv_obj_t *title_label = lv_label_create(box);
    lv_label_set_text(title_label, title);
    lv_obj_set_style_text_font(title_label, MCUI_FONT_16, 0);
    lv_obj_set_style_text_color(title_label, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_width(title_label, SCR_W - 124);
    lv_label_set_long_mode(title_label, LV_LABEL_LONG_WRAP);
    lv_obj_t *details_label = lv_label_create(box);
    lv_label_set_text(details_label, details);
    lv_obj_set_pos(details_label, 0, 80);
    lv_obj_set_width(details_label, SCR_W - 72);
    lv_obj_set_style_text_font(details_label, MCUI_FONT_16, 0);
    lv_obj_set_style_text_color(details_label, lv_color_hex(TH_TEXT), 0);
    lv_obj_t *close = lv_button_create(box);
    lv_obj_set_size(close, 40, 40);
    lv_obj_align(close, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_add_event_cb(close, maps_popup_close, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *close_label = lv_label_create(close);
    lv_label_set_text(close_label, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(close_label, MCUI_FONT_16, 0);
    lv_obj_center(close_label);
}

static void maps_zoom_delta(int delta)
{
    int z = s_map.zoom + delta;
    if (z < kMapMinZoom || z > kMapMaxZoom) return;
    s_map.zoom = z;
    s_map.drag_dx = 0;
    s_map.drag_dy = 0;
    maps_zoom_set_label();
    maps_rebuild(true, true);
    maps_viewport_save_later();
}

static void maps_btn_plus_cb(lv_event_t *) { maps_zoom_delta(+1); }
static void maps_btn_minus_cb(lv_event_t *) { maps_zoom_delta(-1); }
static void maps_btn_center_cb(lv_event_t *)
{
    if (maps_center_from_local_node()) {
        maps_rebuild(true, true);
        maps_viewport_save_later();
    } else {
        maps_status_set("No local position yet");
    }
}

static void maps_names_cb(lv_event_t *event)
{
    s_map.show_names = lv_obj_has_state(static_cast<lv_obj_t *>(lv_event_get_target(event)), LV_STATE_CHECKED);
    if (s_map.tiles_ready) maps_refresh_markers();
    maps_viewport_save_later();
}

lv_obj_t *maps_screen_create(lv_obj_t *parent)
{
    s_map.page = make_page(parent);

    s_map.content = lv_obj_create(s_map.page);
    lv_obj_remove_style_all(s_map.content);
    lv_obj_set_size(s_map.content, SCR_W, PAGE_H);
    lv_obj_set_pos(s_map.content, 0, 0);
    lv_obj_set_style_bg_color(s_map.content, lv_color_hex(0x1B2024), 0);
    lv_obj_set_style_bg_opa(s_map.content, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_map.content, 0, 0);
    lv_obj_set_style_pad_all(s_map.content, 0, 0);
    lv_obj_remove_flag(s_map.content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_map.content, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_map.content, maps_on_pressing, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(s_map.content, maps_on_released, LV_EVENT_RELEASED, nullptr);

    s_map.pan = lv_obj_create(s_map.content);
    lv_obj_remove_style_all(s_map.pan);
    lv_obj_set_size(s_map.pan, SCR_W, PAGE_H);
    lv_obj_set_pos(s_map.pan, 0, 0);
    lv_obj_set_style_bg_opa(s_map.pan, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_map.pan, 0, 0);
    lv_obj_set_style_pad_all(s_map.pan, 0, 0);
    lv_obj_remove_flag(s_map.pan, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_map.pan, LV_OBJ_FLAG_CLICKABLE);

    for (int i = 0; i < kMapMaxTileSlots; i++) {
        s_map_tiles[i].img = lv_image_create(s_map.pan);
        lv_obj_add_flag(s_map_tiles[i].img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_map_tiles[i].img, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(s_map_tiles[i].img, LV_OBJ_FLAG_CLICKABLE);

        s_map_tiles[i].placeholder = lv_obj_create(s_map.pan);
        lv_obj_remove_style_all(s_map_tiles[i].placeholder);
        lv_obj_set_size(s_map_tiles[i].placeholder, kMapTilePx, kMapTilePx);
        lv_obj_set_style_bg_color(s_map_tiles[i].placeholder, lv_color_hex(0x30363B), 0);
        lv_obj_set_style_bg_opa(s_map_tiles[i].placeholder, LV_OPA_40, 0);
        lv_obj_set_style_border_color(s_map_tiles[i].placeholder, lv_color_hex(0x464C50), 0);
        lv_obj_set_style_border_width(s_map_tiles[i].placeholder, 1, 0);
        lv_obj_set_style_radius(s_map_tiles[i].placeholder, 0, 0);
        lv_obj_add_flag(s_map_tiles[i].placeholder, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_map_tiles[i].placeholder, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(s_map_tiles[i].placeholder, LV_OBJ_FLAG_CLICKABLE);

        s_map_tiles[i].src[0] = '\0';

        lv_obj_move_foreground(s_map_tiles[i].img);
    }

    for (int i = 0; i < kMapMaxMarkerSlots; i++) {
        s_map_markers[i].dot = lv_obj_create(s_map.pan);
        lv_obj_remove_style_all(s_map_markers[i].dot);
        lv_obj_set_size(s_map_markers[i].dot, kMapMarkerDotPx, kMapMarkerDotPx);
        lv_obj_set_style_radius(s_map_markers[i].dot, 0, 0);
        lv_obj_set_style_bg_color(s_map_markers[i].dot, lv_color_hex(0x4DA3FF), 0);
        lv_obj_set_style_bg_opa(s_map_markers[i].dot, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_map_markers[i].dot, 1, 0);
        lv_obj_set_style_border_color(s_map_markers[i].dot, lv_color_hex(0xFFFFFF), 0);
        lv_obj_add_flag(s_map_markers[i].dot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_map_markers[i].dot, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(s_map_markers[i].dot, LV_OBJ_FLAG_CLICKABLE);

        s_map_markers[i].tag = lv_obj_create(s_map.pan);
        lv_obj_remove_style_all(s_map_markers[i].tag);
        lv_obj_set_size(s_map_markers[i].tag, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(s_map_markers[i].tag, lv_color_hex(0x62686E), 0);
        lv_obj_set_style_bg_opa(s_map_markers[i].tag, LV_OPA_70, 0);
        lv_obj_set_style_border_color(s_map_markers[i].tag, lv_color_hex(0x9AA0A6), 0);
        lv_obj_set_style_border_opa(s_map_markers[i].tag, LV_OPA_30, 0);
        lv_obj_set_style_border_width(s_map_markers[i].tag, 1, 0);
        lv_obj_set_style_radius(s_map_markers[i].tag, 3, 0);
        lv_obj_set_style_pad_hor(s_map_markers[i].tag, 3, 0);
        lv_obj_set_style_pad_ver(s_map_markers[i].tag, 1, 0);
        lv_obj_add_flag(s_map_markers[i].tag, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_map_markers[i].tag, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(s_map_markers[i].tag, LV_OBJ_FLAG_CLICKABLE);

        s_map_markers[i].tag_label = lv_label_create(s_map_markers[i].tag);
        lv_label_set_text(s_map_markers[i].tag_label, "");
        lv_obj_set_style_text_color(s_map_markers[i].tag_label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_max_width(s_map_markers[i].tag_label, 108, 0);
        lv_obj_set_style_text_font(s_map_markers[i].tag_label, MCUI_FONT_16, 0);
        lv_obj_center(s_map_markers[i].tag_label);

        s_map_markers[i].active = false;
    }

    lv_obj_t *topbar = lv_obj_create(s_map.content);
    lv_obj_remove_style_all(topbar);
    lv_obj_set_size(topbar, SCR_W, 36);
    lv_obj_set_pos(topbar, 0, 0);
    lv_obj_set_style_bg_color(topbar, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(topbar, LV_OPA_40, 0);
    lv_obj_set_style_border_width(topbar, 0, 0);
    lv_obj_set_style_pad_hor(topbar, 8, 0);
    lv_obj_set_style_pad_ver(topbar, 4, 0);
    lv_obj_remove_flag(topbar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(topbar, LV_OBJ_FLAG_FLOATING);

    s_map.status = lv_label_create(topbar);
    lv_label_set_text(s_map.status, "Maps: init");
    lv_obj_set_style_text_color(s_map.status, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(s_map.status, MCUI_FONT_16, 0);
    lv_obj_set_width(s_map.status, SCR_W - 78);
    lv_label_set_long_mode(s_map.status, LV_LABEL_LONG_DOT);
    lv_obj_align(s_map.status, LV_ALIGN_LEFT_MID, 0, 0);

    s_map.zoom_lbl = lv_label_create(topbar);
    lv_label_set_text(s_map.zoom_lbl, "Z11");
    lv_obj_set_style_text_color(s_map.zoom_lbl, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(s_map.zoom_lbl, MCUI_FONT_16, 0);
    lv_obj_align(s_map.zoom_lbl, LV_ALIGN_RIGHT_MID, -6, 0);

    lv_obj_t *btn_plus = lv_button_create(s_map.content);
    lv_obj_set_size(btn_plus, 44, 44);
    lv_obj_align(btn_plus, LV_ALIGN_BOTTOM_RIGHT, -10, -10);
    lv_obj_set_style_bg_color(btn_plus, lv_color_hex(TH_ACCENT), 0);
    lv_obj_set_style_radius(btn_plus, 0, 0);
    lv_obj_add_flag(btn_plus, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_event_cb(btn_plus, maps_btn_plus_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *plus_lbl = lv_label_create(btn_plus);
    lv_label_set_text(plus_lbl, "+");
    lv_obj_set_style_text_color(plus_lbl, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(plus_lbl, MCUI_FONT_16, 0);
    lv_obj_center(plus_lbl);

    lv_obj_t *btn_minus = lv_button_create(s_map.content);
    lv_obj_set_size(btn_minus, 44, 44);
    lv_obj_align(btn_minus, LV_ALIGN_BOTTOM_RIGHT, -62, -10);
    lv_obj_set_style_bg_color(btn_minus, lv_color_hex(TH_SURFACE), 0);
    lv_obj_set_style_radius(btn_minus, 0, 0);
    lv_obj_add_flag(btn_minus, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_event_cb(btn_minus, maps_btn_minus_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *minus_lbl = lv_label_create(btn_minus);
    lv_label_set_text(minus_lbl, "-");
    lv_obj_set_style_text_color(minus_lbl, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(minus_lbl, MCUI_FONT_16, 0);
    lv_obj_center(minus_lbl);

    lv_obj_t *btn_center = lv_button_create(s_map.content);
    lv_obj_set_size(btn_center, 44, 44);
    lv_obj_align(btn_center, LV_ALIGN_BOTTOM_RIGHT, -114, -10);
    lv_obj_set_style_bg_color(btn_center, lv_color_hex(TH_SURFACE), 0);
    lv_obj_set_style_radius(btn_center, 0, 0);
    lv_obj_add_flag(btn_center, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_event_cb(btn_center, maps_btn_center_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *center_lbl = lv_label_create(btn_center);
    lv_label_set_text(center_lbl, LV_SYMBOL_GPS);
    lv_obj_set_style_text_color(center_lbl, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(center_lbl, MCUI_FONT_16, 0);
    lv_obj_center(center_lbl);

    if (!s_map.initialized) {
        if (!maps_viewport_load())
            maps_center_from_local_node();
        s_map.initialized = true;
    }
    maps_zoom_set_label();
    lv_obj_t *names = lv_checkbox_create(s_map.content);
    lv_checkbox_set_text(names, "Names");
    lv_obj_set_size(names, 110, 36);
    lv_obj_set_style_text_font(names, MCUI_FONT_16, 0);
    lv_obj_set_style_text_color(names, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_color(names, lv_color_hex(0x30363B), 0);
    lv_obj_set_style_bg_opa(names, LV_OPA_80, 0);
    lv_obj_set_style_pad_all(names, 8, 0);
    lv_obj_set_style_radius(names, 3, 0);
    lv_obj_align(names, LV_ALIGN_TOP_RIGHT, -10, 46);
    if (s_map.show_names) lv_obj_add_state(names, LV_STATE_CHECKED);
    lv_obj_add_event_cb(names, maps_names_cb, LV_EVENT_VALUE_CHANGED, nullptr);
#if defined(CROWPANEL_DISABLE_MAPS)
    maps_status_set("Maps disabled");
    s_map.first_render_pending = false;
#else
    maps_status_set("Open map tab to load tiles");
    s_map.first_render_pending = true;
#endif
    s_map.last_nodes_refresh_ms = millis();

    return s_map.page;
}

void maps_screen_on_show()
{
#if !defined(CROWPANEL_DISABLE_MAPS)
    if (!s_map.page) return;
    if (maps_wifi_conflicts_with_sd()) {
        s_map.first_render_pending = false;
        s_map_marker_restore_ms = 0;
        if (!s_map_blocked_by_wifi) {
            maps_clear_all_tiles();
            maps_hide_all_markers();
            maps_storage_release();
        }
        s_map_blocked_by_wifi = true;
        maps_status_set("Maps require WiFi off");
        return;
    }
    s_map_blocked_by_wifi = false;
    s_map.first_render_pending = true;
    s_map_marker_restore_ms = 0;
    maps_status_set("Open map tab to load tiles");
#endif
}

void maps_screen_on_hide()
{
#if !defined(CROWPANEL_DISABLE_MAPS)
    maps_popup_close();
    maps_apply_drag_and_rebuild();
    if (s_map.save_pending) maps_viewport_save_now();
    s_map.drag_dx = 0;
    s_map.drag_dy = 0;
    s_map.gesture_travel = 0;
    s_map_marker_restore_ms = 0;
    maps_hide_all_markers();
    // Retain decoded tile sources when the shared boot-time SD mount survives.
#if defined(ARCH_ESP32) && defined(CROWPANEL_DHE04005D)
    if (s_map_sd_own_mount) maps_clear_all_tiles();
#else
    maps_clear_all_tiles();
#endif
    maps_storage_release();
#endif
}

void maps_screen_tick()
{
#if defined(CROWPANEL_DISABLE_MAPS)
    return;
#else
    if (!s_map.page || !s_map.pan) return;

    uint32_t now = millis();
    if (maps_wifi_conflicts_with_sd()) {
        if (!s_map_blocked_by_wifi) {
            maps_screen_on_hide();
            s_map_blocked_by_wifi = true;
        }
        maps_status_set("Maps require WiFi off");
        s_map.first_render_pending = false;
        s_map.last_nodes_refresh_ms = now;
        return;
    }
    s_map_blocked_by_wifi = false;
    if (s_map.save_pending && !s_map.fallback_active && s_map.drag_dx == 0 && s_map.drag_dy == 0 &&
        !Throttle::isWithinTimespanMs(s_map.save_changed_ms, 2000))
        maps_viewport_save_now();

    if (s_map.first_render_pending) {
        s_map.first_render_pending = false;
        maps_rebuild(true, true);
        s_map.last_nodes_refresh_ms = now;
        return;
    }

    if (s_map_marker_restore_ms != 0 &&
        !Throttle::isWithinTimespanMs(s_map_marker_restore_ms, kMapMarkerRestoreDelayMs)) {
        s_map_marker_restore_ms = 0;
        if (s_map.tiles_ready) maps_refresh_markers();
        s_map.last_nodes_refresh_ms = now;
    }

    if (!Throttle::isWithinTimespanMs(s_map.last_nodes_refresh_ms, kMapNodeRefreshMs)) {
        s_map.last_nodes_refresh_ms = now;
        if (s_map.drag_dx == 0 && s_map.drag_dy == 0) {
            if (s_map.tiles_ready) {
                maps_refresh_markers();
                maps_tile_status();
            }
        }
    }
    if (s_map.drag_dx == 0 && s_map.drag_dy == 0) maps_load_next_tile();
#endif
}

}

#endif
