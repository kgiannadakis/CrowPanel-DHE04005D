#if HAS_TFT && USE_MCUI

#include "McScreens.h"
#include "McChatView.h"
#include "../McTheme.h"
#include "../McUI.h"
#include "../data/McMessages.h"
#include "../data/McNodeActions.h"

#include "configuration.h"
#include "mesh/NodeDB.h"
#include "mesh/generated/meshtastic/mesh.pb.h"
#if !MESHTASTIC_EXCLUDE_TRACEROUTE
#include "modules/TraceRouteModule.h"
#endif

#include <cstdio>
#include <cstring>
#include <time.h>

namespace mcui {

static lv_obj_t *s_nodes_page = nullptr;
static lv_obj_t *s_nodes_list = nullptr;
static lv_obj_t *s_node_menu_overlay = nullptr;
static uint32_t s_last_rebuild_ms = 0;
static uint32_t s_last_dynamic_update_ms = 0;
static size_t s_last_num_nodes = 0;
static uint32_t s_last_action_tick = 0;
static NodeNum s_suppress_click_node = 0;
static lv_obj_t *s_trace_status_label = nullptr;
static NodeNum s_trace_node = 0;
static uint32_t s_trace_started_ms = 0;
static String s_trace_last_result;
static String s_trace_target_title;

struct NodeEntry {
    uint32_t node_num;
    char title[40];
    lv_obj_t *line3;
    char line3_text[80];
};
static constexpr int MAX_NODE_CARDS = 64;
static constexpr uint32_t NODE_DYNAMIC_REFRESH_MS = 5000;
static constexpr uint32_t NODE_FULL_REBUILD_MS = 60000;
static NodeEntry s_entries[MAX_NODE_CARDS];
static int s_num_entries = 0;

enum class NodeMenuAction : uint8_t {
    ToggleFavorite,
    TraceRoute,
    DeleteNode,
    Close,
};

static const char *hw_model_name(meshtastic_HardwareModel m)
{
    switch (m) {
    case meshtastic_HardwareModel_TLORA_V2:            return "T-LoRa v2";
    case meshtastic_HardwareModel_TLORA_V1:            return "T-LoRa v1";
    case meshtastic_HardwareModel_TLORA_V2_1_1P6:      return "T-LoRa v2.1";
    case meshtastic_HardwareModel_TBEAM:               return "T-Beam";
    case meshtastic_HardwareModel_HELTEC_V2_0:         return "Heltec v2.0";
    case meshtastic_HardwareModel_HELTEC_V2_1:         return "Heltec v2.1";
    case meshtastic_HardwareModel_HELTEC_V1:           return "Heltec v1";
    case meshtastic_HardwareModel_HELTEC_V3:           return "Heltec v3";
    case meshtastic_HardwareModel_HELTEC_WSL_V3:       return "Heltec WSL v3";
    case meshtastic_HardwareModel_LILYGO_TBEAM_S3_CORE:return "T-Beam S3";
    case meshtastic_HardwareModel_RAK4631:             return "RAK4631";
    case meshtastic_HardwareModel_RAK11200:            return "RAK11200";
    case meshtastic_HardwareModel_STATION_G1:          return "Station G1";
    case meshtastic_HardwareModel_STATION_G2:          return "Station G2";
    case meshtastic_HardwareModel_NANO_G1:             return "Nano G1";
    case meshtastic_HardwareModel_NANO_G1_EXPLORER:    return "Nano G1 Exp";
    case meshtastic_HardwareModel_NANO_G2_ULTRA:       return "Nano G2 Ultra";
    case meshtastic_HardwareModel_T_ECHO:              return "T-Echo";
    case meshtastic_HardwareModel_PORTDUINO:           return "Portduino";
    case meshtastic_HardwareModel_TRACKER_T1000_E:     return "Tracker T1000";
    case meshtastic_HardwareModel_DIY_V1:              return "DIY";
    case meshtastic_HardwareModel_M5STACK:             return "M5Stack";
    case meshtastic_HardwareModel_PICOMPUTER_S3:       return "PicoComputer S3";
    case meshtastic_HardwareModel_UNSET:               return "Unknown";
    default:                                            return "Other";
    }
}

static void fmt_last_heard(uint32_t last_heard, char *out, size_t out_sz)
{
    if (!last_heard) { snprintf(out, out_sz, "—"); return; }
    time_t now = time(nullptr);
    if (now < 1700000000 || (uint32_t)now < last_heard) {
        snprintf(out, out_sz, "—");
        return;
    }
    uint32_t d = (uint32_t)now - last_heard;
    if (d < 15)          snprintf(out, out_sz, "now");
    else if (d < 60)     snprintf(out, out_sz, "%us", (unsigned)d);
    else if (d < 3600)   snprintf(out, out_sz, "%um", (unsigned)(d / 60));
    else if (d < 86400)  snprintf(out, out_sz, "%uh", (unsigned)(d / 3600));
    else                 snprintf(out, out_sz, "%ud", (unsigned)(d / 86400));
}

static void node_card_clicked(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_current_target(e);
    NodeEntry *ent = (NodeEntry *)lv_obj_get_user_data(obj);
    if (!ent || ent->node_num == 0) return;
    if (s_suppress_click_node == ent->node_num) {
        s_suppress_click_node = 0;
        return;
    }
    chatview_open(McConvId::direct(ent->node_num), ent->title);
}

static void node_menu_close()
{
    if (s_node_menu_overlay) {
        lv_obj_delete(s_node_menu_overlay);
        s_node_menu_overlay = nullptr;
    }
    s_trace_status_label = nullptr;
    s_trace_node = 0;
    s_trace_started_ms = 0;
    s_trace_last_result = "";
    s_trace_target_title = "";
}

static String format_traceroute_tx_rx(const String &result)
{
    int split = result.indexOf('\n');
    if (split < 0) {
        if (result == "No response received")
            return "TX: traceroute request sent\n\nRX: No response received";
        return String("TX: ") + result + "\n\nRX: Not available";
    }

    String tx = result.substring(0, split);
    String rx = result.substring(split + 1);
    tx.trim();
    rx.trim();
    if (tx.length() == 0)
        tx = "No outgoing route";
    if (rx.length() == 0)
        rx = "No return path yet";
    return String("TX: ") + tx + "\n\nRX: " + rx;
}

static String trace_popup_status_text()
{
#if !MESHTASTIC_EXCLUDE_TRACEROUTE
    if (!traceRouteModule)
        return "TX: traceroute unavailable\n\nRX: traceroute unavailable";

    TraceRouteRunState state = traceRouteModule->getRunState();
    unsigned long module_start = traceRouteModule->getLastTraceRouteTime();
    bool matches_request = module_start != 0 && (uint32_t)(module_start - s_trace_started_ms) < 0x80000000UL;

    if (matches_request && state == TRACEROUTE_STATE_RESULT && traceRouteModule->getResultText().length() > 0)
        s_trace_last_result = traceRouteModule->getResultText();

    if (s_trace_last_result.length() > 0)
        return format_traceroute_tx_rx(s_trace_last_result);

    if (state == TRACEROUTE_STATE_COOLDOWN) {
        String banner = traceRouteModule->getBannerText();
        if (banner.length() == 0)
            banner = "Traceroute cooldown";
        return String("TX: ") + banner + "\n\nRX: not sent yet";
    }

    if (matches_request && state == TRACEROUTE_STATE_TRACKING)
        return String("TX: traceroute request sent to ") + s_trace_target_title + "\n\nRX: waiting for reply...";

    return String("TX: starting traceroute to ") + s_trace_target_title + "\n\nRX: waiting for reply...";
#else
    return "TX: traceroute module excluded\n\nRX: traceroute module excluded";
#endif
}

static void trace_popup_tick()
{
    if (!s_trace_status_label || s_trace_node == 0)
        return;
    String text = trace_popup_status_text();
    lv_label_set_text(s_trace_status_label, text.c_str());
}

static void trace_popup_close_cb(lv_event_t *e)
{
    (void)e;
    node_menu_close();
}

static void traceroute_popup_open(NodeNum node, const char *title)
{
    node_menu_close();
    s_trace_node = node;
    s_trace_started_ms = millis();
    s_trace_last_result = "";
    s_trace_target_title = title ? title : "node";

    lv_obj_t *scr = lv_screen_active();
    s_node_menu_overlay = lv_obj_create(scr);
    lv_obj_remove_style_all(s_node_menu_overlay);
    lv_obj_set_size(s_node_menu_overlay, SCR_W, SCR_H);
    lv_obj_set_pos(s_node_menu_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_node_menu_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_node_menu_overlay, LV_OPA_60, 0);
    lv_obj_remove_flag(s_node_menu_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(s_node_menu_overlay);

    lv_obj_t *card = lv_obj_create(s_node_menu_overlay);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, SCR_W - 40, 300);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(TH_SURFACE), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 0, 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_set_style_pad_row(card, 10, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);

    char title_buf[88];
    snprintf(title_buf, sizeof(title_buf), "Traceroute\n%s", s_trace_target_title.c_str());
    lv_obj_t *tl = lv_label_create(card);
    lv_label_set_text(tl, title_buf);
    lv_label_set_long_mode(tl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(tl, lv_pct(100));
    lv_obj_set_style_text_color(tl, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(tl, &lv_font_montserrat_16, 0);

    s_trace_status_label = lv_label_create(card);
    lv_label_set_long_mode(s_trace_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_trace_status_label, lv_pct(100));
    lv_obj_set_flex_grow(s_trace_status_label, 1);
    lv_obj_set_style_text_color(s_trace_status_label, lv_color_hex(TH_TEXT2), 0);
    lv_obj_set_style_text_font(s_trace_status_label, &lv_font_montserrat_16, 0);

    lv_obj_t *close = lv_button_create(card);
    lv_obj_set_size(close, lv_pct(100), 46);
    lv_obj_set_style_bg_color(close, lv_color_hex(TH_INPUT), 0);
    lv_obj_set_style_radius(close, 0, 0);
    lv_obj_add_event_cb(close, trace_popup_close_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *l = lv_label_create(close);
    lv_label_set_text(l, "Close");
    lv_obj_set_style_text_color(l, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
    lv_obj_center(l);

    node_actions_traceroute(node);
    trace_popup_tick();
}

static void node_menu_action_cb(lv_event_t *e)
{
    NodeEntry *ent = (NodeEntry *)lv_event_get_user_data(e);
    NodeMenuAction action = (NodeMenuAction)(uintptr_t)lv_obj_get_user_data((lv_obj_t *)lv_event_get_current_target(e));
    if (!ent || ent->node_num == 0) {
        node_menu_close();
        return;
    }

    switch (action) {
    case NodeMenuAction::ToggleFavorite:
        node_actions_set_favorite(ent->node_num, nodeDB ? !nodeDB->isFavorite(ent->node_num) : true);
        break;
    case NodeMenuAction::TraceRoute:
        traceroute_popup_open(ent->node_num, ent->title);
        return;
    case NodeMenuAction::DeleteNode:
        node_actions_delete(ent->node_num);
        break;
    case NodeMenuAction::Close:
        break;
    }
    node_menu_close();
}

static lv_obj_t *add_menu_button(lv_obj_t *parent, const char *label, uint32_t color, NodeEntry *ent, NodeMenuAction action)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, lv_pct(100), 46);
    lv_obj_set_style_bg_color(btn, lv_color_hex(color), 0);
    lv_obj_set_style_radius(btn, 0, 0);
    lv_obj_set_user_data(btn, (void *)(uintptr_t)action);
    lv_obj_add_event_cb(btn, node_menu_action_cb, LV_EVENT_CLICKED, ent);

    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_color(l, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
    lv_obj_center(l);
    return btn;
}

static void node_card_long_pressed(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_current_target(e);
    NodeEntry *ent = (NodeEntry *)lv_obj_get_user_data(obj);
    if (!ent || ent->node_num == 0) return;
    s_suppress_click_node = ent->node_num;

    lv_indev_t *indev = lv_indev_active();
    if (indev)
        lv_indev_wait_release(indev);

    node_menu_close();

    lv_obj_t *scr = lv_screen_active();
    s_node_menu_overlay = lv_obj_create(scr);
    lv_obj_remove_style_all(s_node_menu_overlay);
    lv_obj_set_size(s_node_menu_overlay, SCR_W, SCR_H);
    lv_obj_set_pos(s_node_menu_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_node_menu_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_node_menu_overlay, LV_OPA_60, 0);
    lv_obj_remove_flag(s_node_menu_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(s_node_menu_overlay);

    lv_obj_t *card = lv_obj_create(s_node_menu_overlay);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, SCR_W - 48, 350);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(TH_SURFACE), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 0, 0);
    lv_obj_set_style_pad_all(card, 14, 0);
    lv_obj_set_style_pad_row(card, 8, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);

    char title[72];
    snprintf(title, sizeof(title), "%s\n!%08x", ent->title, (unsigned)ent->node_num);
    lv_obj_t *tl = lv_label_create(card);
    lv_label_set_text(tl, title);
    lv_label_set_long_mode(tl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(tl, lv_pct(100));
    lv_obj_set_style_text_color(tl, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(tl, &lv_font_montserrat_16, 0);

    bool fav = nodeDB && nodeDB->isFavorite(ent->node_num);
    add_menu_button(card, fav ? "Remove from favourite" : "Make favourite", TH_BUBBLE_OUT, ent, NodeMenuAction::ToggleFavorite);
    add_menu_button(card, "Traceroute", TH_ACCENT, ent, NodeMenuAction::TraceRoute);
    add_menu_button(card, "Delete node", 0xB83232, ent, NodeMenuAction::DeleteNode);
    add_menu_button(card, "Cancel", TH_INPUT, ent, NodeMenuAction::Close);
}

static void fmt_node_metrics(const meshtastic_NodeInfoLite *n, char *out, size_t out_sz)
{
    int16_t rssi = node_rssi_get(n->num);
    char lh[12];
    fmt_last_heard(n->last_heard, lh, sizeof(lh));
    if (rssi != 0) {
        snprintf(out, out_sz, "SNR %.1f  RSSI %d  ·  %s", n->snr, (int)rssi, lh);
    } else {
        snprintf(out, out_sz, "SNR %.1f  ·  %s", n->snr, lh);
    }
}

static void add_node_card(NodeEntry *ent, const meshtastic_NodeInfoLite *n)
{
    lv_obj_t *card = lv_obj_create(s_nodes_list);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_height(card, 74);
    lv_obj_set_style_bg_color(card, lv_color_hex(TH_SURFACE), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 0, 0);
    lv_obj_set_style_pad_all(card, 10, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(card, ent);
    lv_obj_add_event_cb(card, node_card_clicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(card, node_card_long_pressed, LV_EVENT_LONG_PRESSED, nullptr);

    lv_obj_t *dot = lv_obj_create(card);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 44, 44);
    lv_obj_set_pos(dot, 0, 4);
    lv_obj_set_style_radius(dot, 0, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(TH_BUBBLE_OUT), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);

    char short_name[6] = "?";
    if (n->has_user && n->user.short_name[0]) {
        strncpy(short_name, n->user.short_name, sizeof(short_name) - 1);
        short_name[sizeof(short_name) - 1] = '\0';
    }
    lv_obj_t *dl = lv_label_create(dot);
    lv_label_set_text(dl, short_name);
    lv_obj_set_style_text_color(dl, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(dl, &lv_font_montserrat_16, 0);
    lv_obj_center(dl);

    lv_obj_t *tl = lv_label_create(card);
    lv_label_set_text(tl, ent->title);
    lv_obj_set_style_text_color(tl, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_text_font(tl, &lv_font_montserrat_16, 0);
    lv_obj_set_pos(tl, 56, 2);
    lv_obj_set_width(tl, SCR_W - 56 - 20);
    lv_label_set_long_mode(tl, LV_LABEL_LONG_DOT);

    char line2[64];
    const char *hw = hw_model_name(n->has_user ? n->user.hw_model : meshtastic_HardwareModel_UNSET);
    snprintf(line2, sizeof(line2), "%s!%08x  %s", n->is_favorite ? "FAV  " : "", (unsigned)n->num, hw);
    lv_obj_t *l2 = lv_label_create(card);
    lv_label_set_text(l2, line2);
    lv_obj_set_style_text_color(l2, lv_color_hex(TH_TEXT2), 0);
    lv_obj_set_style_text_font(l2, &lv_font_montserrat_16, 0);
    lv_obj_set_pos(l2, 56, 22);
    lv_obj_set_width(l2, SCR_W - 56 - 20);
    lv_label_set_long_mode(l2, LV_LABEL_LONG_DOT);

    char line3[80];
    fmt_node_metrics(n, line3, sizeof(line3));
    lv_obj_t *l3 = lv_label_create(card);
    lv_label_set_text(l3, line3);
    ent->line3 = l3;
    strncpy(ent->line3_text, line3, sizeof(ent->line3_text) - 1);
    ent->line3_text[sizeof(ent->line3_text) - 1] = '\0';
    lv_obj_set_style_text_color(l3, lv_color_hex(TH_TEXT3), 0);
    lv_obj_set_style_text_font(l3, &lv_font_montserrat_16, 0);
    lv_obj_set_pos(l3, 56, 44);
    lv_obj_set_width(l3, SCR_W - 56 - 20);
    lv_label_set_long_mode(l3, LV_LABEL_LONG_DOT);
}

static void update_node_metric_labels()
{
    if (!nodeDB) return;
    for (int i = 0; i < s_num_entries; i++) {
        NodeEntry *ent = &s_entries[i];
        if (!ent->line3 || ent->node_num == 0)
            continue;

        meshtastic_NodeInfoLite *node = nodeDB->getMeshNode(ent->node_num);
        if (!node)
            continue;

        char line3[80];
        fmt_node_metrics(node, line3, sizeof(line3));
        if (strncmp(line3, ent->line3_text, sizeof(ent->line3_text)) != 0) {
            lv_label_set_text(ent->line3, line3);
            strncpy(ent->line3_text, line3, sizeof(ent->line3_text) - 1);
            ent->line3_text[sizeof(ent->line3_text) - 1] = '\0';
        }
    }
}

static void add_section_header(const char *text)
{
    lv_obj_t *h = lv_label_create(s_nodes_list);
    lv_label_set_text(h, text);
    lv_obj_set_style_text_color(h, lv_color_hex(TH_TEXT3), 0);
    lv_obj_set_style_text_font(h, &lv_font_montserrat_16, 0);
    lv_obj_set_style_pad_top(h, 8, 0);
    lv_obj_set_style_pad_left(h, 12, 0);
}

static void rebuild_nodes_list()
{
    if (!s_nodes_list || !nodeDB) return;
    lv_obj_clean(s_nodes_list);
    s_num_entries = 0;

    auto count_nodes_matching = [&](bool want_favorite) {
        int count = 0;
        NodeNum ours = nodeDB->getNodeNum();
        size_t n = nodeDB->getNumMeshNodes();
        for (size_t i = 0; i < n; i++) {
            meshtastic_NodeInfoLite *node = nodeDB->getMeshNodeByIndex(i);
            if (!node) continue;
            if (node->num == ours) continue;
            if (node->is_favorite != want_favorite) continue;
            count++;
        }
        return count;
    };

    auto add_nodes_matching = [&](bool want_favorite) {
        NodeNum ours = nodeDB->getNodeNum();
        size_t n = nodeDB->getNumMeshNodes();
        for (size_t i = 0; i < n && s_num_entries < MAX_NODE_CARDS; i++) {
            meshtastic_NodeInfoLite *node = nodeDB->getMeshNodeByIndex(i);
            if (!node) continue;
            if (node->num == ours) continue;
            if (node->is_favorite != want_favorite) continue;

            NodeEntry *ent = &s_entries[s_num_entries++];
            ent->node_num = node->num;

            const char *title = nullptr;
            char fallback[16];
            if (node->has_user) {
                if (node->user.long_name[0])
                    title = node->user.long_name;
                else if (node->user.short_name[0])
                    title = node->user.short_name;
            }
            if (!title) {
                snprintf(fallback, sizeof(fallback), "!%08x", (unsigned)node->num);
                title = fallback;
            }
            strncpy(ent->title, title, sizeof(ent->title) - 1);
            ent->title[sizeof(ent->title) - 1] = '\0';

            add_node_card(ent, node);
        }
    };

    int favorite_count = count_nodes_matching(true);
    int other_count = count_nodes_matching(false);
    if (favorite_count > 0) {
        add_section_header("Favourites");
        add_nodes_matching(true);
    }
    if (other_count > 0 && s_num_entries < MAX_NODE_CARDS) {
        add_section_header("Nodes");
        add_nodes_matching(false);
    }

    if (s_num_entries == 0) {
        lv_obj_t *empty = lv_label_create(s_nodes_list);
        lv_label_set_text(empty, "No nodes heard yet.");
        lv_obj_set_style_text_color(empty, lv_color_hex(TH_TEXT3), 0);
        lv_obj_set_style_pad_top(empty, 40, 0);
    }

    s_last_num_nodes = nodeDB->getNumMeshNodes();
    s_last_rebuild_ms = millis();
    s_last_dynamic_update_ms = s_last_rebuild_ms;
}

lv_obj_t *nodes_screen_create(lv_obj_t *parent)
{
    s_nodes_page = lv_obj_create(parent);
    lv_obj_remove_style_all(s_nodes_page);
    lv_obj_set_size(s_nodes_page, SCR_W, PAGE_H);
    lv_obj_set_pos(s_nodes_page, 0, 0);
    lv_obj_set_style_bg_color(s_nodes_page, lv_color_hex(TH_BG), 0);
    lv_obj_set_style_bg_opa(s_nodes_page, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_nodes_page, LV_OBJ_FLAG_SCROLLABLE);

    s_nodes_list = lv_obj_create(s_nodes_page);
    lv_obj_remove_style_all(s_nodes_list);
    lv_obj_set_size(s_nodes_list, SCR_W, PAGE_H);
    lv_obj_set_pos(s_nodes_list, 0, 0);
    lv_obj_set_style_bg_color(s_nodes_list, lv_color_hex(TH_BG), 0);
    lv_obj_set_style_bg_opa(s_nodes_list, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_nodes_list, 8, 0);
    lv_obj_set_style_pad_row(s_nodes_list, 6, 0);
    lv_obj_set_flex_flow(s_nodes_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_nodes_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_nodes_list, LV_SCROLLBAR_MODE_AUTO);

    rebuild_nodes_list();
    s_last_action_tick = node_actions_change_tick();
    return s_nodes_page;
}

void nodes_screen_tick()
{
    trace_popup_tick();
    if (!s_nodes_list || !nodeDB) return;
    uint32_t now = millis();
    size_t n = nodeDB->getNumMeshNodes();
    uint32_t action_tick = node_actions_change_tick();
    bool need_rebuild = (n != s_last_num_nodes) || (action_tick != s_last_action_tick) || (now - s_last_rebuild_ms > NODE_FULL_REBUILD_MS);
    if (need_rebuild)
        s_last_action_tick = action_tick;
    if (need_rebuild) {
        rebuild_nodes_list();
    } else if (now - s_last_dynamic_update_ms > NODE_DYNAMIC_REFRESH_MS) {
        update_node_metric_labels();
        s_last_dynamic_update_ms = now;
    }
}

}

#endif
