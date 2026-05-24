// ui_homescreen.h — Programmatic homescreen declarations

#ifndef UI_HOMESCREEN_H
#define UI_HOMESCREEN_H

#ifdef __cplusplus
extern "C" {
#endif

// SCREEN: ui_homescreen
extern void ui_homescreen_screen_init(void);
extern void ui_homescreen_screen_destroy(void);
extern lv_obj_t * ui_homescreen;
extern lv_obj_t * ui_devicenamepanel;
extern lv_obj_t * ui_devicenamelabel;
extern lv_obj_t * ui_chatpanel;
extern lv_obj_t * ui_mutebutton;
extern lv_obj_t * ui_mutelabel;
extern lv_obj_t * ui_searchfield;
extern lv_obj_t * ui_textsendtype;
extern lv_obj_t * ui_backbutton;
extern lv_obj_t * ui_backlabel;
extern lv_obj_t * ui_timelabel;
extern lv_obj_t * ui_Keyboard1;

// Stubs kept for link-compat — main.cpp may reference these nav buttons
extern lv_obj_t * ui_homebutton1;
extern lv_obj_t * ui_homeabel1;
extern lv_obj_t * ui_settingsbutton1;
extern lv_obj_t * ui_settingslabel1;

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif
