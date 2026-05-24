// features_cb.cpp — Callbacks for the Features & Bridges screen

#include "app_globals.h"
#include "features_ui.h"
#include "features_cb.h"
#include "settings_cb.h"
#include "ui_settingscreen.h"
#include "ui_helpers.h"
#include "display.h"
#include "wifi_ntp.h"
#include "telegram_bridge.h"
#include "web_dashboard.h"
#include "ota_update.h"
#include "translate.h"
#include "utils.h"

static lv_coord_t s_form_orig_h = 0;

// Forward: back button navigates to settings
static void cb_features_back(lv_event_t*) {
    _ui_screen_change(&ui_settingscreen, LV_SCR_LOAD_ANIM_NONE, 0, 0,
                       ui_settingscreen_screen_init);
}

// ── Telegram toggle ─────────────────────────────────────────

static void cb_tg_toggle(lv_event_t*) {
    // Read text areas in case user didn't defocus before toggling
    if (ui_tg_token_ta) {
        const char* txt = lv_textarea_get_text(ui_tg_token_ta);
        tgbridge_set_token(txt ? txt : "");
    }
    if (ui_tg_chatid_ta) {
        const char* txt = lv_textarea_get_text(ui_tg_chatid_ta);
        tgbridge_set_chat_id(txt ? txt : "");
    }
    g_tgbridge_enabled = !g_tgbridge_enabled;
    tgbridge_save_settings();
    g_deferred_features_dirty = true;
    serialmon_append(g_tgbridge_enabled ? "Telegram bridge: ON" : "Telegram bridge: OFF");
}

void features_field_focus(lv_obj_t* field) {
    if (!field || !ui_FeaturesKB) return;
    kb_show(ui_FeaturesKB, field);
    lv_obj_t* form = lv_obj_get_parent(field);
    if (!form) return;
    if (s_form_orig_h == 0) s_form_orig_h = lv_obj_get_height(form);
    lv_coord_t form_y = lv_obj_get_y(form);
    lv_coord_t kb_top = SETTINGS_KB_TOP;
    lv_coord_t new_h = kb_top - form_y;
    if (new_h > 100) lv_obj_set_height(form, new_h);
    lv_obj_scroll_to_view(field, LV_ANIM_ON);
}

void features_field_defocus(lv_obj_t* field) {
    kb_hide(ui_FeaturesKB, field);
    if (!field) return;
    lv_obj_t* form = lv_obj_get_parent(field);
    if (form && s_form_orig_h > 0) {
        lv_obj_set_height(form, s_form_orig_h);
        s_form_orig_h = 0;
    }
}

static void cb_tg_token_focus(lv_event_t* e) {
    features_field_focus(ui_tg_token_ta);
}

static void cb_tg_token_defocus(lv_event_t* e) {
    features_field_defocus(ui_tg_token_ta);
    const char* txt = lv_textarea_get_text(ui_tg_token_ta);
    tgbridge_set_token(txt ? txt : "");
}

static void cb_tg_chatid_focus(lv_event_t* e) {
    features_field_focus(ui_tg_chatid_ta);
}

static void cb_tg_chatid_defocus(lv_event_t* e) {
    features_field_defocus(ui_tg_chatid_ta);
    const char* txt = lv_textarea_get_text(ui_tg_chatid_ta);
    tgbridge_set_chat_id(txt ? txt : "");
}

// ── Web dashboard toggle ────────────────────────────────────

static void cb_wd_toggle(lv_event_t*) {
    g_webdash_enabled = !g_webdash_enabled;
    if (g_webdash_enabled) webdash_start();
    else                   webdash_stop();
    webdash_save_settings();
    g_deferred_features_dirty = true;
    serialmon_append(g_webdash_enabled ? "Web dashboard: ON" : "Web dashboard: OFF");
}

// ── OTA ─────────────────────────────────────────────────────

static void cb_ota_repo_focus(lv_event_t* e) {
    features_field_focus(ui_ota_repo_ta);
}

static void cb_ota_repo_defocus(lv_event_t* e) {
    features_field_defocus(ui_ota_repo_ta);
    const char* txt = lv_textarea_get_text(ui_ota_repo_ta);
    ota_set_repo(txt ? txt : "");
}

static void cb_ota_check(lv_event_t*) {
    ota_check_for_update();
}

// ── KB ready callback ───────────────────────────────────────

static void cb_kb_ready(lv_event_t* e) {
    lv_obj_t* ta = lv_keyboard_get_textarea(ui_FeaturesKB);
    if (ta == ui_tg_token_ta)       cb_tg_token_defocus(NULL);
    else if (ta == ui_tg_chatid_ta) cb_tg_chatid_defocus(NULL);
    else if (ta == ui_ota_repo_ta)  cb_ota_repo_defocus(NULL);
    else                            features_field_defocus(ta);
}

// ── Register all callbacks ──────────────────────────────────

extern "C" void features_register_callbacks() {
    if (!ui_featuresscreen) return;

    // Guard: only register once
    static bool s_registered = false;
    if (s_registered) {
        // Just refresh values and labels on re-entry
        tgbridge_populate_ui();
        ota_populate_ui();
        features_update_status_labels();
        return;
    }
    s_registered = true;

    // Back button is the first child of the header (second child of screen, first btn)
    lv_obj_t* hdr = lv_obj_get_child(ui_featuresscreen, 0);
    if (hdr) {
        lv_obj_t* back_btn = lv_obj_get_child(hdr, 0);
        if (back_btn) lv_obj_add_event_cb(back_btn, cb_features_back, LV_EVENT_CLICKED, NULL);
    }

    // Telegram
    if (ui_tg_toggle)    lv_obj_add_event_cb(ui_tg_toggle, cb_tg_toggle, LV_EVENT_CLICKED, NULL);
    if (ui_tg_token_ta)  lv_obj_add_event_cb(ui_tg_token_ta, cb_tg_token_focus, LV_EVENT_FOCUSED, NULL);
    if (ui_tg_token_ta)  lv_obj_add_event_cb(ui_tg_token_ta, cb_tg_token_defocus, LV_EVENT_DEFOCUSED, NULL);
    if (ui_tg_chatid_ta) lv_obj_add_event_cb(ui_tg_chatid_ta, cb_tg_chatid_focus, LV_EVENT_FOCUSED, NULL);
    if (ui_tg_chatid_ta) lv_obj_add_event_cb(ui_tg_chatid_ta, cb_tg_chatid_defocus, LV_EVENT_DEFOCUSED, NULL);

    // Web dashboard
    if (ui_wd_toggle)    lv_obj_add_event_cb(ui_wd_toggle, cb_wd_toggle, LV_EVENT_CLICKED, NULL);

    // OTA
    if (ui_ota_repo_ta)  lv_obj_add_event_cb(ui_ota_repo_ta, cb_ota_repo_focus, LV_EVENT_FOCUSED, NULL);
    if (ui_ota_repo_ta)  lv_obj_add_event_cb(ui_ota_repo_ta, cb_ota_repo_defocus, LV_EVENT_DEFOCUSED, NULL);
    if (ui_ota_check_btn) lv_obj_add_event_cb(ui_ota_check_btn, cb_ota_check, LV_EVENT_CLICKED, NULL);

    // Keyboard: apply same custom maps/handler as homepage
    if (ui_FeaturesKB) {
        setup_keyboard(ui_FeaturesKB);
        lv_obj_add_event_cb(ui_FeaturesKB, cb_kb_ready, LV_EVENT_READY, NULL);
    }

    // WiFi callbacks (implementations in settings_cb.cpp)
    reg(ui_wifitoggle,       cb_wifi_toggle);
    reg(ui_wifiscanbutton,   cb_wifi_scan);
    reg(ui_wificonnectbutton, cb_wifi_connect);
    reg(ui_wififorgetbutton,  cb_wifi_forget);
    reg(ui_wifipassword,     cb_wifi_password_ready, LV_EVENT_READY);
    reg(ui_wifipassword,     cb_wifi_password_focused, LV_EVENT_FOCUSED);
    reg(ui_wifipassword,     cb_wifi_password_defocused, LV_EVENT_DEFOCUSED);
    ui_apply_wifi_state();
    wifi_ui_update_status();
    btn_lbl(ui_wifiscanbutton, LV_SYMBOL_REFRESH " Scan");
    btn_lbl(ui_wificonnectbutton, LV_SYMBOL_OK " Connect");
    btn_lbl(ui_wififorgetbutton, LV_SYMBOL_TRASH " Forget");

    // Translation callbacks
    reg(ui_autotranslate_toggle, cb_auto_translate_toggle);
    btn_lbl(ui_autotranslate_toggle, LV_SYMBOL_LOOP " Auto-Translate");
    if (ui_translate_lang_dd) {
        lv_dropdown_set_options(ui_translate_lang_dd, translate_lang_list());
        lv_dropdown_set_selected(ui_translate_lang_dd, g_translate_lang_idx);
        reg(ui_translate_lang_dd, cb_translate_lang_changed, LV_EVENT_VALUE_CHANGED);
    }
    ui_apply_auto_translate_state();

    // Set initial label text
    if (ui_tg_toggle_lbl) lv_label_set_text(ui_tg_toggle_lbl, LV_SYMBOL_WIFI " Telegram Bridge");
    if (ui_wd_toggle_lbl) lv_label_set_text(ui_wd_toggle_lbl, LV_SYMBOL_EYE_OPEN " Web Dashboard");
    if (ui_ota_check_lbl) lv_label_set_text(ui_ota_check_lbl, LV_SYMBOL_DOWNLOAD " Check for Update");

    // Load current values into text areas
    tgbridge_populate_ui();
    ota_populate_ui();

    // Refresh status labels
    features_update_status_labels();
}

// ── Status label refresh (called from main loop) ────────────

extern "C" void features_update_status_labels() {
    // Telegram toggle color
    if (ui_tg_toggle) {
        lv_obj_set_style_bg_color(ui_tg_toggle,
            lv_color_hex(g_tgbridge_enabled ? TH_GREEN : TH_SURFACE2), 0);
    }
    if (ui_tg_status_lbl) {
        lv_label_set_text(ui_tg_status_lbl, tgbridge_status_text());
    }

    // Web dashboard toggle color
    if (ui_wd_toggle) {
        lv_obj_set_style_bg_color(ui_wd_toggle,
            lv_color_hex(g_webdash_enabled ? TH_GREEN : TH_SURFACE2), 0);
    }
    if (ui_wd_status_lbl) {
        lv_label_set_text(ui_wd_status_lbl, webdash_status_text());
    }

    // OTA
    if (ui_ota_status_lbl) {
        lv_label_set_text(ui_ota_status_lbl, ota_status_text());
    }
    if (ui_ota_progress_bar) {
        uint8_t pct = ota_progress_percent();
        if (ota_is_updating()) {
            lv_obj_clear_flag(ui_ota_progress_bar, LV_OBJ_FLAG_HIDDEN);
            lv_bar_set_value(ui_ota_progress_bar, pct, LV_ANIM_ON);
        } else {
            lv_obj_add_flag(ui_ota_progress_bar, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (ui_ota_check_btn) {
        if (ota_is_checking() || ota_is_updating()) {
            lv_obj_add_state(ui_ota_check_btn, LV_STATE_DISABLED);
        } else {
            lv_obj_clear_state(ui_ota_check_btn, LV_STATE_DISABLED);
        }
    }
}
