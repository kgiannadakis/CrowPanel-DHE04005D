// =============================================================================
// Dual-Boot Selector — CrowPanel DHE04005D (ESP32-P4)
// Flashed to "factory" partition. Shows two touch buttons on boot.
// Saves last choice to NVS for auto-boot with 3-second countdown.
//
// Port of the DHE04005D (ESP32-S3 + LovyanGFX) selector to ESP32-P4. Display
// is now LVGL on top of esp_lcd_panel_rgb (display.cpp); backlight is the
// STC8 PWM channel (no I2C 0x30 chip on this board); WiFi is ESP-Hosted via
// the on-board ESP32-C6 (assumed pre-flashed with esp-hosted firmware).
//
// Architecture: LVGL drives a dedicated FreeRTOS task. UI events post deferred
// "actions" to globals; the Arduino main loop() picks them up and runs the
// blocking work (WiFi connect, HTTPS fetch, OTA write). UI mutations from the
// main loop are bracketed with lvgl_lock() / lvgl_unlock().
// =============================================================================

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <esp_app_format.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>

#include <lvgl.h>

#include "board_config.h"
#include "display.h"
#include "i2c_bus.h"
#include "stc8.h"

// ---------------------------------------------------------------------------
// Layout constants (post scan-offset compensation: LVGL sees 792×479)
// ---------------------------------------------------------------------------
static constexpr int SCR_W = H_size - 8;     // 792
static constexpr int SCR_H = V_size - 1;     // 479

// Firmware buttons — two big, side-by-side
static constexpr int BTN_W = 350, BTN_H = 170, BTN_GAP = 40;
static constexpr int BTN_Y = 110;
static constexpr int BTN_A_X = (SCR_W - 2*BTN_W - BTN_GAP) / 2;
static constexpr int BTN_B_X = BTN_A_X + BTN_W + BTN_GAP;

// Action buttons (Update / WiFi)
static constexpr int ACTION_W = 260, ACTION_H = 58, ACTION_GAP = 30;
static constexpr int ACTION_Y = 305;
static constexpr int UPDATE_X = (SCR_W - 2*ACTION_W - ACTION_GAP) / 2;
static constexpr int WIFI_X   = UPDATE_X + ACTION_W + ACTION_GAP;

// Colors (RGB hex; LVGL converts to RGB565)
static constexpr uint32_t COL_BG         = 0x0B1A30;
static constexpr uint32_t COL_HEADER     = 0x1A1A2E;
static constexpr uint32_t COL_MESHCORE   = 0x2ECC71;
static constexpr uint32_t COL_MESHTASTIC = 0x3498DB;
static constexpr uint32_t COL_UPDATE     = 0x9B7BFF;
static constexpr uint32_t COL_WIFI       = 0xE74C3C;
static constexpr uint32_t COL_DISABLED   = 0x1A1A2E;
static constexpr uint32_t COL_TEXT       = 0xFFFFFF;
static constexpr uint32_t COL_SUB        = 0xA0B4C8;
static constexpr uint32_t COL_WARN       = 0xFFB020;
static constexpr uint32_t COL_ERR        = 0xFF4444;
static constexpr uint32_t COL_OK         = 0x33DD77;
static constexpr uint32_t COL_BTN_GREY   = 0x34495E;
static constexpr uint32_t COL_BTN_DARK   = 0x233A5E;
static constexpr uint32_t COL_BTN_GREEN2 = 0x27AE60;
static constexpr uint32_t COL_BTN_GREY2  = 0x7F8C8D;

// ---------------------------------------------------------------------------
// NVS / OTA / GitHub
// ---------------------------------------------------------------------------
static const char *WIFI_NS     = "selwifi";
static const char *DUALBOOT_NS = "dualboot";
static const char *GITHUB_API  = "https://api.github.com/repos/kgiannadakis/CrowPanel-DIS02050A/releases/latest";
static const char *UA          = "CrowPanel-DHE04005D-Selector/2.0";
// TODO: repoint GITHUB_API + asset_score() heuristics at the DHE04005D
// release repo once it exists. Until then OTA will report "Could not find
// release assets" — that's the expected error path.

static Preferences prefs;

struct FirmwareAssets {
    String tag;
    String meshcoreUrl;
    String meshtasticUrl;
};

// ---------------------------------------------------------------------------
// Deferred action plumbing — event callbacks (running in LVGL task) post one
// of these; loop() (Arduino task) consumes them.
// ---------------------------------------------------------------------------
enum Action : uint8_t {
    ACT_NONE = 0,
    ACT_BOOT_SLOT_0,
    ACT_BOOT_SLOT_1,
    ACT_GO_MAIN,
    ACT_GO_WIFI_MENU,
    ACT_GO_UPDATE_CONFIRM,
    ACT_DO_UPDATE,
    ACT_WIFI_SCAN,
    ACT_WIFI_CONNECT_SAVED,
    ACT_WIFI_FORGET,
    ACT_WIFI_PASSWORD_SUBMIT,
    ACT_WIFI_PICK_NETWORK,         // user tapped a row in the scan list
    ACT_BACK,
    ACT_CANCEL_AUTOBOOT,
};

static volatile Action s_pending_action = ACT_NONE;
static int  s_pending_int       = 0;          // index for ACT_WIFI_PICK_NETWORK
static char s_pending_password[80] = "";       // result of password keyboard

static void post_action(Action a) { s_pending_action = a; }
static void post_action_i(Action a, int v) { s_pending_int = v; s_pending_action = a; }

// ---------------------------------------------------------------------------
// Backlight — STC8 PWM channel 0 (no I2C 0x30 chip on this board)
// ---------------------------------------------------------------------------
static bool s_update_backlight_dimmed = false;

static void selector_backlight_on() {
    stc8_set_pwm_duty(STC8_PWM_LCD_BL_EN, 100);
    s_update_backlight_dimmed = false;
}
static void selector_backlight_off() {
    stc8_set_pwm_duty(STC8_PWM_LCD_BL_EN, 0);
    s_update_backlight_dimmed = true;
}

// ---------------------------------------------------------------------------
// Partition helpers (unchanged from S3)
// ---------------------------------------------------------------------------
static const esp_partition_t* find_ota(int slot) {
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP,
        slot == 0 ? ESP_PARTITION_SUBTYPE_APP_OTA_0 : ESP_PARTITION_SUBTYPE_APP_OTA_1, NULL);
}

static bool has_firmware(const esp_partition_t* p) {
    if (!p) return false;
    uint8_t h[4]; esp_partition_read(p, 0, h, 4);
    return h[0] == 0xE9;
}

static bool firmware_version(const esp_partition_t* p, char *out, size_t out_sz) {
    if (!out || out_sz == 0) return false;
    out[0] = '\0';
    if (!p || !has_firmware(p)) return false;
    esp_app_desc_t desc = {};
    if (esp_ota_get_partition_description(p, &desc) != ESP_OK) return false;
    if (!desc.version[0]) return false;
    snprintf(out, out_sz, "v%s", desc.version);
    return true;
}

// ---------------------------------------------------------------------------
// LVGL helpers
// ---------------------------------------------------------------------------
static lv_color_t col(uint32_t rgb) { return lv_color_hex(rgb); }

// Tag-based event router — encode (action, payload) into the user_data ptr
// and have a single callback dispatch.
static void btn_event_cb(lv_event_t* e) {
    Action a = (Action)(uintptr_t)lv_event_get_user_data(e);
    post_action(a);
}

static void net_row_event_cb(lv_event_t* e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    post_action_i(ACT_WIFI_PICK_NETWORK, idx);
}

static lv_obj_t* make_button(lv_obj_t* parent, int x, int y, int w, int h,
                             uint32_t bg_rgb, const char* label,
                             Action on_click, bool enabled = true,
                             const char* sub = nullptr,
                             const lv_font_t* font = &lv_font_montserrat_20) {
    lv_obj_t* btn = lv_btn_create(parent);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_radius(btn, 14, 0);
    lv_obj_set_style_bg_color(btn, col(enabled ? bg_rgb : COL_DISABLED), 0);
    lv_obj_set_style_border_width(btn, enabled ? 0 : 1, 0);
    lv_obj_set_style_border_color(btn, col(0x333333), 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    if (enabled && on_click != ACT_NONE) {
        lv_obj_add_event_cb(btn, btn_event_cb, LV_EVENT_CLICKED, (void*)(uintptr_t)on_click);
    } else {
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    }

    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_color(lbl, col(enabled ? COL_TEXT : 0x666666), 0);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_align(lbl, sub ? LV_ALIGN_CENTER : LV_ALIGN_CENTER, 0, sub ? -10 : 0);

    if (sub) {
        lv_obj_t* sublbl = lv_label_create(btn);
        lv_label_set_text(sublbl, sub);
        lv_obj_set_style_text_color(sublbl, col(enabled ? 0xD0D0D0 : 0x444444), 0);
        lv_obj_set_style_text_font(sublbl, &lv_font_montserrat_14, 0);
        lv_obj_align(sublbl, LV_ALIGN_CENTER, 0, 18);
    }

    if (!enabled) {
        lv_obj_t* dis = lv_label_create(btn);
        lv_label_set_text(dis, "[empty slot]");
        lv_obj_set_style_text_color(dis, col(0xFF4444), 0);
        lv_obj_set_style_text_font(dis, &lv_font_montserrat_14, 0);
        lv_obj_align(dis, LV_ALIGN_BOTTOM_MID, 0, -10);
    }
    return btn;
}

static void make_back_button(lv_obj_t* parent) {
    make_button(parent, 20, 410, 150, 50, COL_BTN_GREY, "Back", ACT_BACK,
                true, nullptr, &lv_font_montserrat_16);
}

static void make_screen_header(lv_obj_t* scr, const char* title, const char* subtitle) {
    lv_obj_set_style_bg_color(scr, col(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t* hdr = lv_obj_create(scr);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_set_size(hdr, SCR_W, 70);
    lv_obj_set_style_bg_color(hdr, col(COL_HEADER), 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_radius(hdr, 0, 0);
    lv_obj_set_style_pad_all(hdr, 0, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* tlbl = lv_label_create(hdr);
    lv_label_set_text(tlbl, title);
    lv_obj_set_style_text_color(tlbl, col(COL_TEXT), 0);
    lv_obj_set_style_text_font(tlbl, &lv_font_montserrat_22, 0);
    lv_obj_align(tlbl, LV_ALIGN_CENTER, 0, subtitle ? -12 : 0);

    if (subtitle) {
        lv_obj_t* slbl = lv_label_create(hdr);
        lv_label_set_text(slbl, subtitle);
        lv_obj_set_style_text_color(slbl, col(COL_SUB), 0);
        lv_obj_set_style_text_font(slbl, &lv_font_montserrat_14, 0);
        lv_obj_align(slbl, LV_ALIGN_CENTER, 0, 16);
    }
}

// ---------------------------------------------------------------------------
// Screens
// ---------------------------------------------------------------------------
static lv_obj_t* s_screen_main           = nullptr;
static lv_obj_t* s_screen_wifi_menu      = nullptr;
static lv_obj_t* s_screen_wifi_list      = nullptr;
static lv_obj_t* s_screen_wifi_password  = nullptr;
static lv_obj_t* s_screen_update_confirm = nullptr;
static lv_obj_t* s_screen_status         = nullptr;

static lv_obj_t* s_main_countdown_lbl = nullptr;
static lv_obj_t* s_password_textarea  = nullptr;
static lv_obj_t* s_password_keyboard  = nullptr;

static String s_pending_ssid;          // SSID picked in network list
static bool   s_pending_open_network = false;

static void switch_screen(lv_obj_t* scr) {
    lv_scr_load(scr);
}

// --- Main screen (firmware buttons + countdown) ----------------------------
static lv_obj_t* build_screen_main(bool ota0_ok, bool ota1_ok) {
    lv_obj_t* scr = lv_obj_create(NULL);
    make_screen_header(scr,
        "MeshCore/Meshtastic Dual-Boot by KaA",
        "Tap a firmware to boot  |  Auto-boots last choice in 3s");

    char meshcoreVer[40], meshtasticVer[40];
    const char* mcSub = firmware_version(find_ota(0), meshcoreVer, sizeof(meshcoreVer)) ? meshcoreVer : nullptr;
    const char* mtSub = firmware_version(find_ota(1), meshtasticVer, sizeof(meshtasticVer)) ? meshtasticVer : nullptr;

    make_button(scr, BTN_A_X, BTN_Y, BTN_W, BTN_H, COL_MESHCORE,   "MeshCore",   ACT_BOOT_SLOT_0, ota0_ok, mcSub, &lv_font_montserrat_28);
    make_button(scr, BTN_B_X, BTN_Y, BTN_W, BTN_H, COL_MESHTASTIC, "Meshtastic", ACT_BOOT_SLOT_1, ota1_ok, mtSub, &lv_font_montserrat_28);

    // keep for future reference: hide Update Firmware and WiFi Setup buttons on the main screen
    // make_button(scr, UPDATE_X, ACTION_Y, ACTION_W, ACTION_H, COL_UPDATE, "Update Firmware", ACT_GO_UPDATE_CONFIRM, true, nullptr, &lv_font_montserrat_18);
    // make_button(scr, WIFI_X,   ACTION_Y, ACTION_W, ACTION_H, COL_WIFI,   "WiFi Setup",      ACT_GO_WIFI_MENU,      true, nullptr, &lv_font_montserrat_18);

    s_main_countdown_lbl = lv_label_create(scr);
    lv_label_set_text(s_main_countdown_lbl, "");
    lv_obj_set_style_text_color(s_main_countdown_lbl, col(COL_SUB), 0);
    lv_obj_set_style_text_font(s_main_countdown_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(s_main_countdown_lbl, LV_ALIGN_BOTTOM_MID, 0, -40);

    lv_obj_t* footer = lv_label_create(scr);
    lv_label_set_text(footer, "Bootloader 2.0 (P4 port)  |  Baked by Kostis Giannadakis");
    lv_obj_set_style_text_color(footer, col(COL_TEXT), 0);
    lv_obj_set_style_text_font(footer, &lv_font_montserrat_14, 0);
    lv_obj_align(footer, LV_ALIGN_BOTTOM_MID, 0, -10);

    return scr;
}

// --- WiFi menu screen ------------------------------------------------------
static lv_obj_t* build_screen_wifi_menu() {
    lv_obj_t* scr = lv_obj_create(NULL);
    make_screen_header(scr, "WiFi Setup", "Connect the selector to your network");

    String savedSsid, savedPass;
    prefs.begin(WIFI_NS, true);
    savedSsid = prefs.getString("ssid", "");
    savedPass = prefs.getString("pass", "");
    prefs.end();
    bool hasSaved = savedSsid.length() > 0;

    make_button(scr, 95, 115, 285, 70, COL_WIFI, "Scan Networks",  ACT_WIFI_SCAN, true, nullptr, &lv_font_montserrat_20);
    make_button(scr, 420, 115, 285, 70, COL_BTN_GREEN2, "Connect Saved",
                ACT_WIFI_CONNECT_SAVED, hasSaved,
                hasSaved ? savedSsid.c_str() : "none saved",
                &lv_font_montserrat_20);
    make_button(scr, 95, 215, 285, 70, COL_BTN_GREY2, "Forget Saved", ACT_WIFI_FORGET, hasSaved, nullptr, &lv_font_montserrat_20);
    make_button(scr, 420, 215, 285, 70, COL_BTN_GREY, "Back", ACT_GO_MAIN, true, nullptr, &lv_font_montserrat_20);

    bool connected = WiFi.status() == WL_CONNECTED;
    lv_obj_t* st = lv_label_create(scr);
    String line = connected
        ? "WiFi: " + WiFi.SSID() + "  |  IP: " + WiFi.localIP().toString()
        : "WiFi is not connected";
    lv_label_set_text(st, line.c_str());
    lv_obj_set_style_text_color(st, col(connected ? COL_OK : COL_SUB), 0);
    lv_obj_set_style_text_font(st, &lv_font_montserrat_14, 0);
    lv_obj_align(st, LV_ALIGN_TOP_MID, 0, 320);

    return scr;
}

// --- WiFi network list screen ---------------------------------------------
struct ScanResult {
    String ssid;
    int32_t rssi;
    uint8_t enc;
};
static ScanResult s_scan_results[8];
static int        s_scan_count = 0;

static void rescan_btn_cb(lv_event_t*) { post_action(ACT_WIFI_SCAN); }

static lv_obj_t* build_screen_wifi_list() {
    lv_obj_t* scr = lv_obj_create(NULL);
    make_screen_header(scr, "WiFi Setup", "Select a network");

    if (s_scan_count == 0) {
        lv_obj_t* lbl = lv_label_create(scr);
        lv_label_set_text(lbl, "No networks found. Tap Rescan or Back.");
        lv_obj_set_style_text_color(lbl, col(COL_WARN), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, -30);
    } else {
        for (int i = 0; i < s_scan_count; i++) {
            int y = 86 + i * 40;
            lv_obj_t* row = lv_btn_create(scr);
            lv_obj_set_pos(row, 55, y);
            lv_obj_set_size(row, 690, 36);
            lv_obj_set_style_radius(row, 8, 0);
            lv_obj_set_style_bg_color(row, col(0x162A48), 0);
            lv_obj_set_style_shadow_width(row, 0, 0);
            lv_obj_set_style_border_width(row, 0, 0);
            lv_obj_add_event_cb(row, net_row_event_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);

            lv_obj_t* nm = lv_label_create(row);
            String label = s_scan_results[i].ssid.length() ? s_scan_results[i].ssid : "(hidden network)";
            if (label.length() > 34) label = label.substring(0, 34) + "...";
            lv_label_set_text(nm, label.c_str());
            lv_obj_set_style_text_color(nm, col(COL_TEXT), 0);
            lv_obj_set_style_text_font(nm, &lv_font_montserrat_16, 0);
            lv_obj_align(nm, LV_ALIGN_LEFT_MID, 10, 0);

            lv_obj_t* meta = lv_label_create(row);
            String m = String(s_scan_results[i].rssi) + " dBm";
            if (s_scan_results[i].enc == WIFI_AUTH_OPEN) m += "  open";
            lv_label_set_text(meta, m.c_str());
            lv_obj_set_style_text_color(meta, col(COL_SUB), 0);
            lv_obj_set_style_text_font(meta, &lv_font_montserrat_14, 0);
            lv_obj_align(meta, LV_ALIGN_RIGHT_MID, -10, 0);
        }
    }

    lv_obj_t* rescan = lv_btn_create(scr);
    lv_obj_set_pos(rescan, 560, 410);
    lv_obj_set_size(rescan, 150, 50);
    lv_obj_set_style_bg_color(rescan, col(COL_BTN_GREY), 0);
    lv_obj_set_style_radius(rescan, 14, 0);
    lv_obj_set_style_shadow_width(rescan, 0, 0);
    lv_obj_add_event_cb(rescan, rescan_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* rl = lv_label_create(rescan);
    lv_label_set_text(rl, "Rescan");
    lv_obj_set_style_text_color(rl, col(COL_TEXT), 0);
    lv_obj_set_style_text_font(rl, &lv_font_montserrat_16, 0);
    lv_obj_center(rl);

    make_back_button(scr);
    return scr;
}

// --- WiFi password screen --------------------------------------------------
static void password_kb_event_cb(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t* kb = lv_event_get_target(e);
    if (code == LV_EVENT_READY) {
        // user tapped Connect (the OK key)
        const char* txt = lv_textarea_get_text(s_password_textarea);
        strncpy(s_pending_password, txt ? txt : "", sizeof(s_pending_password)-1);
        s_pending_password[sizeof(s_pending_password)-1] = '\0';
        post_action(ACT_WIFI_PASSWORD_SUBMIT);
    } else if (code == LV_EVENT_CANCEL) {
        post_action(ACT_GO_WIFI_MENU);
    }
    (void)kb;
}

static lv_obj_t* build_screen_wifi_password(const String& ssid) {
    lv_obj_t* scr = lv_obj_create(NULL);
    make_screen_header(scr, "WiFi Password", ssid.c_str());

    s_password_textarea = lv_textarea_create(scr);
    lv_obj_set_pos(s_password_textarea, 50, 80);
    lv_obj_set_size(s_password_textarea, 700, 50);
    lv_textarea_set_one_line(s_password_textarea, true);
    lv_textarea_set_password_mode(s_password_textarea, true);
    lv_textarea_set_placeholder_text(s_password_textarea, "Tap to type password");
    lv_obj_set_style_text_font(s_password_textarea, &lv_font_montserrat_18, 0);

    s_password_keyboard = lv_keyboard_create(scr);
    lv_obj_set_size(s_password_keyboard, SCR_W, 280);
    lv_obj_align(s_password_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(s_password_keyboard, s_password_textarea);
    lv_keyboard_set_mode(s_password_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_add_event_cb(s_password_keyboard, password_kb_event_cb, LV_EVENT_ALL, NULL);

    return scr;
}

// --- Update confirm screen -------------------------------------------------
static lv_obj_t* build_screen_update_confirm() {
    lv_obj_t* scr = lv_obj_create(NULL);
    make_screen_header(scr, "Update Firmware", nullptr);

    auto add_line = [&](const char* txt, int y, uint32_t color, const lv_font_t* f) {
        lv_obj_t* l = lv_label_create(scr);
        lv_label_set_text(l, txt);
        lv_obj_set_style_text_color(l, col(color), 0);
        lv_obj_set_style_text_font(l, f, 0);
        lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
    };
    add_line("You are about to update the device's firmware.", 90,  COL_TEXT, &lv_font_montserrat_18);
    add_line("Your settings might be lost!",                   140, COL_ERR,  &lv_font_montserrat_22);
    add_line("The screen will turn dark for a few minutes.",   200, COL_TEXT, &lv_font_montserrat_16);
    add_line("Do not turn it off or unplug it until",          230, COL_TEXT, &lv_font_montserrat_16);
    add_line("you see the successfully updated screen.",       258, COL_TEXT, &lv_font_montserrat_16);
    add_line("Press Continue to proceed.",                     310, COL_WARN, &lv_font_montserrat_18);

    make_back_button(scr);
    make_button(scr, 560, 410, 210, 50, COL_OK, "Continue", ACT_DO_UPDATE, true, nullptr, &lv_font_montserrat_16);
    return scr;
}

// --- Status screen (errors / connecting / success) -------------------------
static lv_obj_t* s_status_back_btn = nullptr;

static lv_obj_t* build_screen_status(const char* title, const char* l1,
                                     const char* l2, uint32_t color, bool with_back) {
    lv_obj_t* scr = lv_obj_create(NULL);
    make_screen_header(scr, title, nullptr);

    lv_obj_t* la = lv_label_create(scr);
    lv_label_set_text(la, l1 ? l1 : "");
    lv_obj_set_style_text_color(la, col(color), 0);
    lv_obj_set_style_text_font(la, &lv_font_montserrat_20, 0);
    lv_obj_align(la, LV_ALIGN_CENTER, 0, -10);
    lv_label_set_long_mode(la, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(la, SCR_W - 80);
    lv_obj_set_style_text_align(la, LV_TEXT_ALIGN_CENTER, 0);

    if (l2 && l2[0]) {
        lv_obj_t* lb = lv_label_create(scr);
        lv_label_set_text(lb, l2);
        lv_obj_set_style_text_color(lb, col(COL_SUB), 0);
        lv_obj_set_style_text_font(lb, &lv_font_montserrat_16, 0);
        lv_obj_align(lb, LV_ALIGN_CENTER, 0, 30);
        lv_label_set_long_mode(lb, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(lb, SCR_W - 80);
        lv_obj_set_style_text_align(lb, LV_TEXT_ALIGN_CENTER, 0);
    }

    s_status_back_btn = nullptr;
    if (with_back) {
        s_status_back_btn = lv_btn_create(scr);
        lv_obj_set_pos(s_status_back_btn, 20, 410);
        lv_obj_set_size(s_status_back_btn, 150, 50);
        lv_obj_set_style_bg_color(s_status_back_btn, col(COL_BTN_GREY), 0);
        lv_obj_set_style_radius(s_status_back_btn, 14, 0);
        lv_obj_set_style_shadow_width(s_status_back_btn, 0, 0);
        lv_obj_add_event_cb(s_status_back_btn, btn_event_cb, LV_EVENT_CLICKED, (void*)(uintptr_t)ACT_BACK);
        lv_obj_t* lb = lv_label_create(s_status_back_btn);
        lv_label_set_text(lb, "Back");
        lv_obj_set_style_text_color(lb, col(COL_TEXT), 0);
        lv_obj_set_style_text_font(lb, &lv_font_montserrat_16, 0);
        lv_obj_center(lb);
    }

    return scr;
}

static void show_status(const char* title, const String& l1,
                        const String& l2, uint32_t color, bool with_back) {
    bool reveal_after_draw = s_update_backlight_dimmed;
    if (!lvgl_lock(-1)) return;
    if (s_screen_status) { lv_obj_del(s_screen_status); s_screen_status = nullptr; }
    s_screen_status = build_screen_status(title, l1.c_str(), l2.c_str(), color, with_back);
    lv_scr_load(s_screen_status);
    lvgl_unlock();
    if (reveal_after_draw) {
        delay(40);
        selector_backlight_on();
    }
}

// ---------------------------------------------------------------------------
// Boot a slot and reboot
// ---------------------------------------------------------------------------
static void boot_slot(int slot) {
    const esp_partition_t* p = find_ota(slot);
    if (!p || !has_firmware(p)) {
        show_status("Boot", "No firmware in this slot.", "", COL_ERR, false);
        delay(1800);
        return;
    }
    prefs.begin(DUALBOOT_NS, false);
    prefs.putInt("last", slot);
    prefs.end();

    show_status("Boot", String("Booting ") + (slot == 0 ? "MeshCore..." : "Meshtastic..."), "", COL_OK, false);
    delay(400);
    esp_ota_set_boot_partition(p);
    esp_restart();
}

// ---------------------------------------------------------------------------
// Auto-boot countdown timer (LVGL-task-side)
// ---------------------------------------------------------------------------
static int s_countdown_remaining = 0;
static int s_countdown_slot      = -1;
static lv_timer_t* s_countdown_timer = nullptr;

static void countdown_timer_cb(lv_timer_t* t) {
    s_countdown_remaining--;
    char msg[80];
    if (s_countdown_remaining > 0) {
        snprintf(msg, sizeof(msg), "Auto-booting %s in %ds (touch to cancel)",
                 s_countdown_slot == 0 ? "MeshCore" : "Meshtastic",
                 s_countdown_remaining);
        lv_label_set_text(s_main_countdown_lbl, msg);
    } else {
        lv_timer_del(t);
        s_countdown_timer = nullptr;
        post_action(s_countdown_slot == 0 ? ACT_BOOT_SLOT_0 : ACT_BOOT_SLOT_1);
    }
}

static void cancel_countdown() {
    if (s_countdown_timer) {
        lv_timer_del(s_countdown_timer);
        s_countdown_timer = nullptr;
    }
    if (s_main_countdown_lbl) {
        lv_label_set_text(s_main_countdown_lbl, "Auto-boot cancelled. Tap a button.");
    }
}

// Catch any touch on the main screen to cancel the countdown.
static void main_screen_press_cb(lv_event_t*) {
    if (s_countdown_timer) post_action(ACT_CANCEL_AUTOBOOT);
}

static void start_countdown(int slot) {
    s_countdown_slot      = slot;
    s_countdown_remaining = 3;
    char msg[80];
    snprintf(msg, sizeof(msg), "Auto-booting %s in %ds (touch to cancel)",
             slot == 0 ? "MeshCore" : "Meshtastic", s_countdown_remaining);
    lv_label_set_text(s_main_countdown_lbl, msg);
    s_countdown_timer = lv_timer_create(countdown_timer_cb, 1000, NULL);
    lv_obj_add_event_cb(s_screen_main, main_screen_press_cb, LV_EVENT_PRESSED, NULL);
}

// ---------------------------------------------------------------------------
// WiFi
// ---------------------------------------------------------------------------
static bool load_wifi(String &ssid, String &pass) {
    prefs.begin(WIFI_NS, true);
    ssid = prefs.getString("ssid", "");
    pass = prefs.getString("pass", "");
    prefs.end();
    return ssid.length() > 0;
}

static void save_wifi(const String &ssid, const String &pass) {
    prefs.begin(WIFI_NS, false);
    prefs.putString("ssid", ssid);
    prefs.putString("pass", pass);
    prefs.end();
}

static void forget_wifi() {
    prefs.begin(WIFI_NS, false);
    prefs.clear();
    prefs.end();
    WiFi.disconnect(true, true);
}

static void wifi_setpins_once() {
    static bool done = false;
    if (done) return;
    // ESP-Hosted SDIO link from P4 to the on-board ESP32-C6.
    WiFi.setPins(
        WIFI_HOSTED_SDIO_PIN_CLK,
        WIFI_HOSTED_SDIO_PIN_CMD,
        WIFI_HOSTED_SDIO_PIN_D0,
        WIFI_HOSTED_SDIO_PIN_D1,
        WIFI_HOSTED_SDIO_PIN_D2,
        WIFI_HOSTED_SDIO_PIN_D3,
        WIFI_HOSTED_SDIO_PIN_RESET
    );
    done = true;
}

static bool connect_wifi(const String &ssid, const String &pass, bool saveOnSuccess) {
    show_status("WiFi", "Connecting to " + ssid + "...", "", COL_SUB, false);
    wifi_setpins_once();
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(false, false);
    delay(200);
    WiFi.begin(ssid.c_str(), pass.c_str());

    for (int i = 0; i < 30; i++) {
        if (WiFi.status() == WL_CONNECTED) {
            if (saveOnSuccess) save_wifi(ssid, pass);
            show_status("WiFi", "Connected to " + ssid,
                        WiFi.localIP().toString(), COL_OK, true);
            return true;
        }
        delay(500);
    }
    show_status("WiFi", "Connection failed.",
                "Check the password and try again.", COL_ERR, true);
    return false;
}

static bool connect_saved_wifi_silent() {
    if (WiFi.status() == WL_CONNECTED) return true;
    String ssid, pass;
    if (!load_wifi(ssid, pass)) return false;
    wifi_setpins_once();
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(false, false);
    delay(200);
    WiFi.begin(ssid.c_str(), pass.c_str());
    for (int i = 0; i < 40; i++) {
        if (WiFi.status() == WL_CONNECTED) return true;
        delay(500);
    }
    return false;
}

static int do_scan(ScanResult out[], int maxItems) {
    show_status("WiFi Setup", "Scanning networks...", "", COL_SUB, false);
    wifi_setpins_once();
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(false, false);
    delay(200);
    int n = WiFi.scanNetworks(false, true);
    if (n <= 0) return 0;
    int count = n < maxItems ? n : maxItems;
    for (int i = 0; i < count; i++) {
        out[i].ssid = WiFi.SSID(i);
        out[i].rssi = WiFi.RSSI(i);
        out[i].enc  = WiFi.encryptionType(i);
    }
    WiFi.scanDelete();
    return count;
}

// ---------------------------------------------------------------------------
// GitHub release JSON parsing + asset matching (unchanged from S3)
// ---------------------------------------------------------------------------
static bool extract_json_string(const String &json, const String &key,
                                int start, String &out, int *endPos = nullptr) {
    int keyPos = json.indexOf("\"" + key + "\"", start);
    if (keyPos < 0) return false;
    int colon = json.indexOf(':', keyPos);
    int q1 = json.indexOf('"', colon + 1);
    if (colon < 0 || q1 < 0) return false;
    out = "";
    bool esc = false;
    for (int i = q1 + 1; i < (int)json.length(); i++) {
        char c = json[i];
        if (esc) {
            if (c == 'n') out += '\n';
            else if (c == 't') out += '\t';
            else out += c;
            esc = false;
        } else if (c == '\\') {
            esc = true;
        } else if (c == '"') {
            if (endPos) *endPos = i + 1;
            return true;
        } else {
            out += c;
        }
    }
    return false;
}

// TODO(DHE04005D OTA): re-tune asset_score() heuristics for the P4 release
// asset names once the new repo exists. For now we keep the v11 heuristics
// so the code path still compiles + is testable end-to-end.
static int asset_score(const String &name, bool meshcore) {
    String n = name;
    n.toLowerCase();
    if (!n.endsWith(".bin")) return -1000;
    if (n.indexOf("bootloader") >= 0 || n.indexOf("partition") >= 0 ||
        n.indexOf("selector") >= 0 || n.indexOf("factory") >= 0 ||
        n.indexOf("littlefs") >= 0 || n.indexOf("spiffs") >= 0) {
        return -1000;
    }
    if (meshcore) {
        if (n == "meshcore.bin") return 100;
        int s = 0;
        if (n.indexOf("meshcore") >= 0) s += 50;
        if (n.indexOf("crowpanel") >= 0) s += 10;
        if (n.indexOf("dhe04005d") >= 0 || n.indexOf("p4") >= 0) s += 25;
        return s;
    }
    if (n == "meshtastic.bin") return 100;
    int s = 0;
    if (n.indexOf("meshtastic") >= 0) s += 50;
    if (n.indexOf("dhe04005d") >= 0 || n.indexOf("p4") >= 0) s += 25;
    if (n.indexOf("firmware") >= 0) s += 5;
    return s;
}

static bool fetch_latest_assets(FirmwareAssets &assets) {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

    if (!http.begin(client, GITHUB_API)) return false;
    http.addHeader("User-Agent", UA);
    http.addHeader("Accept", "application/vnd.github+json");
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        http.end();
        show_status("Update Firmware", "GitHub request failed.",
                    "HTTP " + String(code), COL_ERR, true);
        return false;
    }
    String json = http.getString();
    http.end();

    extract_json_string(json, "tag_name", 0, assets.tag);

    int bestMesh = -1000, bestMt = -1000;
    int pos = 0;
    while (true) {
        int urlKey = json.indexOf("\"browser_download_url\"", pos);
        if (urlKey < 0) break;
        String url;
        int next = 0;
        if (!extract_json_string(json, "browser_download_url", urlKey, url, &next)) break;
        int nameKey = json.lastIndexOf("\"name\"", urlKey);
        String name;
        if (nameKey >= 0 && extract_json_string(json, "name", nameKey, name)) {
            int ms = asset_score(name, true);
            int mts = asset_score(name, false);
            if (ms > bestMesh) { bestMesh = ms; assets.meshcoreUrl = url; }
            if (mts > bestMt)  { bestMt   = mts; assets.meshtasticUrl = url; }
        }
        pos = next > urlKey ? next : urlKey + 1;
    }
    return bestMesh > 0 && bestMt > 0;
}

static bool write_url_to_partition(const char *label, const String &url,
                                   const esp_partition_t *part) {
    if (!part) {
        show_status("Update Firmware", String(label) + " partition not found.", "", COL_ERR, true);
        return false;
    }
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setTimeout(20000);
    if (!http.begin(client, url)) {
        show_status("Update Firmware", String(label) + " download could not start.", "", COL_ERR, true);
        return false;
    }
    http.addHeader("User-Agent", UA);
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        http.end();
        show_status("Update Firmware", String(label) + " download failed.",
                    "HTTP " + String(code), COL_ERR, true);
        return false;
    }
    int len = http.getSize();
    if (len > 0 && (uint32_t)len > part->size) {
        http.end();
        show_status("Update Firmware", String(label) + " is too large.",
                    String(len) + " bytes > slot " + String(part->size), COL_ERR, true);
        return false;
    }
    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(part, OTA_SIZE_UNKNOWN, &handle);
    if (err != ESP_OK) {
        http.end();
        show_status("Update Firmware", String(label) + " erase failed.",
                    "esp_ota_begin: " + String((int)err), COL_ERR, true);
        return false;
    }
    WiFiClient *stream = http.getStreamPtr();
    uint8_t *buf = (uint8_t *)malloc(4096);
    if (!buf) {
        esp_ota_abort(handle);
        http.end();
        show_status("Update Firmware", "Out of memory.", "", COL_ERR, true);
        return false;
    }
    size_t written = 0;
    uint32_t lastData = millis();
    bool ok = true;
    while (http.connected() && (len < 0 || written < (size_t)len)) {
        size_t avail = stream->available();
        if (avail) {
            int rd = stream->readBytes(buf, avail > 4096 ? 4096 : avail);
            if (rd <= 0) continue;
            err = esp_ota_write(handle, buf, rd);
            if (err != ESP_OK) { ok = false; break; }
            written += rd;
            lastData = millis();
        } else {
            if (millis() - lastData > 25000) { ok = false; break; }
            delay(10);
        }
    }
    free(buf);
    http.end();
    if (!ok || written == 0) {
        esp_ota_abort(handle);
        show_status("Update Firmware", String(label) + " write failed.",
                    String(written / 1024) + " KB written", COL_ERR, true);
        return false;
    }
    err = esp_ota_end(handle);
    if (err != ESP_OK || !has_firmware(part)) {
        show_status("Update Firmware", String(label) + " validation failed.",
                    "esp_ota_end: " + String((int)err), COL_ERR, true);
        return false;
    }
    return true;
}

static void update_firmware_screen() {
    show_status("Update Firmware", "Update in progress.",
                "Do not unplug the device.", COL_WARN, false);
    delay(1500);
    selector_backlight_off();

    if (!connect_saved_wifi_silent()) {
        show_status("Update Firmware", "WiFi connection failed.",
                    "Open WiFi Setup and try again.", COL_ERR, true);
        return;
    }
    FirmwareAssets assets;
    if (!fetch_latest_assets(assets)) {
        show_status("Update Firmware", "Could not find release assets.",
                    "Expected meshcore.bin and meshtastic.bin.", COL_ERR, true);
        return;
    }
    if (!write_url_to_partition("MeshCore", assets.meshcoreUrl, find_ota(0))) return;
    if (!write_url_to_partition("Meshtastic", assets.meshtasticUrl, find_ota(1))) return;

    selector_backlight_on();
    show_status("Update Firmware", "Update successful.",
                "Rebooting in 10 seconds...", COL_OK, false);
    delay(10000);
    esp_restart();
}

// ---------------------------------------------------------------------------
// Main-loop action dispatcher
// ---------------------------------------------------------------------------
static void rebuild_main_screen() {
    bool ota0_ok = has_firmware(find_ota(0));
    bool ota1_ok = has_firmware(find_ota(1));
    if (!lvgl_lock(-1)) return;
    if (s_screen_main) { lv_obj_del(s_screen_main); }
    s_screen_main = build_screen_main(ota0_ok, ota1_ok);
    lv_scr_load(s_screen_main);
    lvgl_unlock();
}

static void go_main()      { rebuild_main_screen(); }
static void go_wifi_menu() {
    if (!lvgl_lock(-1)) return;
    if (s_screen_wifi_menu) { lv_obj_del(s_screen_wifi_menu); }
    s_screen_wifi_menu = build_screen_wifi_menu();
    lv_scr_load(s_screen_wifi_menu);
    lvgl_unlock();
}
static void go_update_confirm() {
    if (!lvgl_lock(-1)) return;
    if (s_screen_update_confirm) { lv_obj_del(s_screen_update_confirm); }
    s_screen_update_confirm = build_screen_update_confirm();
    lv_scr_load(s_screen_update_confirm);
    lvgl_unlock();
}
static void go_wifi_list() {
    if (!lvgl_lock(-1)) return;
    if (s_screen_wifi_list) { lv_obj_del(s_screen_wifi_list); }
    s_screen_wifi_list = build_screen_wifi_list();
    lv_scr_load(s_screen_wifi_list);
    lvgl_unlock();
}
static void go_wifi_password(const String& ssid) {
    if (!lvgl_lock(-1)) return;
    if (s_screen_wifi_password) { lv_obj_del(s_screen_wifi_password); }
    s_screen_wifi_password = build_screen_wifi_password(ssid);
    lv_scr_load(s_screen_wifi_password);
    lvgl_unlock();
}

// Where Back goes depends on which screen the user is on.
static void handle_back() {
    lv_obj_t* cur = lv_scr_act();
    if (cur == s_screen_status) {
        // Status screens land back at main by default (caller of show_status
        // controls whether Back is offered at all).
        go_main();
    } else if (cur == s_screen_wifi_list || cur == s_screen_wifi_password) {
        go_wifi_menu();
    } else if (cur == s_screen_wifi_menu || cur == s_screen_update_confirm) {
        go_main();
    } else {
        go_main();
    }
}

// =============================================================================
// Setup
// =============================================================================
void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("[selector] CrowPanel DHE04005D dual-boot selector booting");

    // I2C bus + STC8 (backlight) up FIRST so display / touch can use them.
    i2c1_bus_handle();
    stc8_set_pwm_duty(STC8_PWM_LCD_BL_EN, 100);

    // Display + touch
    if (!display_init()) {
        Serial.println("[selector] display_init failed — halting");
        while (true) delay(1000);
    }
    if (!display_touch_attach()) {
        Serial.println("[selector] display_touch_attach failed — continuing without touch");
    }

    // Pre-warm WiFi via ESP-Hosted (don't block long if C6 isn't there)
    wifi_setpins_once();
    String ssid, pass;
    if (load_wifi(ssid, pass)) {
        WiFi.mode(WIFI_STA);
        WiFi.begin(ssid.c_str(), pass.c_str());
        uint32_t start = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start < 1500) delay(50);
    }

    // Build main screen (LVGL widgets) — safe before lvgl task starts.
    bool ota0_ok = has_firmware(find_ota(0));
    bool ota1_ok = has_firmware(find_ota(1));
    s_screen_main = build_screen_main(ota0_ok, ota1_ok);
    lv_scr_load(s_screen_main);

    // Auto-boot countdown if a previous slot was saved
    int last;
    prefs.begin(DUALBOOT_NS, true);
    last = prefs.getInt("last", -1);
    prefs.end();
    if (last >= 0 && last <= 1 && has_firmware(find_ota(last))) {
        start_countdown(last);
    }

    // Start the LVGL task — from here on every UI mutation needs lvgl_lock.
    display_start_lvgl_task();
}

// =============================================================================
// Main loop — drains deferred actions
// =============================================================================
void loop() {
    if (s_pending_action == ACT_NONE) {
        delay(10);
        return;
    }

    Action a = s_pending_action;
    int    arg = s_pending_int;
    s_pending_action = ACT_NONE;
    s_pending_int    = 0;

    switch (a) {
        case ACT_BOOT_SLOT_0:        boot_slot(0); break;
        case ACT_BOOT_SLOT_1:        boot_slot(1); break;
        case ACT_GO_MAIN:            go_main(); break;
        case ACT_GO_WIFI_MENU:       go_wifi_menu(); break;
        case ACT_GO_UPDATE_CONFIRM:  go_update_confirm(); break;
        case ACT_BACK:               handle_back(); break;
        case ACT_CANCEL_AUTOBOOT:
            if (lvgl_lock(50)) { cancel_countdown(); lvgl_unlock(); }
            break;

        case ACT_DO_UPDATE:
            update_firmware_screen();
            rebuild_main_screen();
            break;

        case ACT_WIFI_SCAN: {
            int n = do_scan(s_scan_results, 8);
            s_scan_count = n;
            go_wifi_list();
            break;
        }
        case ACT_WIFI_PICK_NETWORK: {
            if (arg < 0 || arg >= s_scan_count) { go_wifi_menu(); break; }
            s_pending_ssid = s_scan_results[arg].ssid;
            s_pending_open_network = (s_scan_results[arg].enc == WIFI_AUTH_OPEN);
            if (s_pending_open_network) {
                connect_wifi(s_pending_ssid, "", true);
            } else {
                go_wifi_password(s_pending_ssid);
            }
            break;
        }
        case ACT_WIFI_PASSWORD_SUBMIT: {
            String pw(s_pending_password);
            connect_wifi(s_pending_ssid, pw, true);
            break;
        }
        case ACT_WIFI_CONNECT_SAVED: {
            String s, p;
            if (load_wifi(s, p)) connect_wifi(s, p, false);
            else                 show_status("WiFi", "No saved WiFi network.", "Open WiFi Setup first.", COL_WARN, true);
            break;
        }
        case ACT_WIFI_FORGET:
            forget_wifi();
            show_status("WiFi Setup", "Saved WiFi credentials removed.", "", COL_OK, true);
            break;

        default: break;
    }
}
