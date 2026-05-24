// features_ui.h — Features screen widget declarations
#ifndef FEATURES_UI_H
#define FEATURES_UI_H

#ifdef __cplusplus
extern "C" {
#endif

extern lv_obj_t* ui_featuresscreen;

void ui_featuresscreen_screen_init(void);
void ui_featuresscreen_screen_destroy(void);

// Translation widgets
extern lv_obj_t* ui_autotranslate_toggle;
extern lv_obj_t* ui_autotranslate_lbl;
extern lv_obj_t* ui_translate_lang_dd;

// Telegram widgets
extern lv_obj_t* ui_tg_toggle;
extern lv_obj_t* ui_tg_toggle_lbl;
extern lv_obj_t* ui_tg_token_ta;
extern lv_obj_t* ui_tg_chatid_ta;
extern lv_obj_t* ui_tg_status_lbl;

// Web dashboard widgets
extern lv_obj_t* ui_wd_toggle;
extern lv_obj_t* ui_wd_toggle_lbl;
extern lv_obj_t* ui_wd_status_lbl;

// OTA widgets
extern lv_obj_t* ui_ota_repo_ta;
extern lv_obj_t* ui_ota_check_btn;
extern lv_obj_t* ui_ota_check_lbl;
extern lv_obj_t* ui_ota_status_lbl;
extern lv_obj_t* ui_ota_progress_bar;

// WiFi widgets
extern lv_obj_t* ui_wifitoggle;
extern lv_obj_t* ui_wifitoggle_lbl;
extern lv_obj_t* ui_wifiscanbutton;
extern lv_obj_t* ui_wifiscan_lbl;
extern lv_obj_t* ui_wifinetworksdropdown;
extern lv_obj_t* ui_wifipassword;
extern lv_obj_t* ui_wificonnectbutton;
extern lv_obj_t* ui_wificonnect_lbl;
extern lv_obj_t* ui_wififorgetbutton;
extern lv_obj_t* ui_wififorget_lbl;
extern lv_obj_t* ui_wifistatuslabel;

// Features keyboard
extern lv_obj_t* ui_FeaturesKB;

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif
