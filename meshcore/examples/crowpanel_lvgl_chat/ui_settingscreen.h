// ui_settingscreen.h — Programmatic settings screen declarations

#ifndef UI_SETTINGSCREEN_H
#define UI_SETTINGSCREEN_H

#ifdef __cplusplus
extern "C" {
#endif

extern void ui_settingscreen_screen_init(void);
extern void ui_settingscreen_screen_destroy(void);
extern lv_obj_t * ui_settingscreen;
extern lv_obj_t * ui_presetpickbutton;
extern lv_obj_t * ui_Label5;
extern lv_obj_t * ui_brightnessslider;
extern lv_obj_t * ui_zeroadvertbutton;
extern lv_obj_t * ui_Label8;
extern lv_obj_t * ui_fadvertbutton;
extern lv_obj_t * ui_Label9;
extern lv_obj_t * ui_positionadverttoggle;
extern lv_obj_t * ui_positionadvert_lbl;
extern lv_obj_t * ui_Label10;
extern lv_obj_t * ui_autocontacttoggle;
extern lv_obj_t * ui_Label11;
extern lv_obj_t * ui_presetsdropdown;
extern lv_obj_t * ui_serialmonitorwindow;
extern lv_obj_t * ui_Label15;
extern lv_obj_t * ui_screentimeout;
extern lv_obj_t * ui_Label1;
extern lv_obj_t * ui_hashtagchannel;
extern lv_obj_t * ui_notificationstoggle;
extern lv_obj_t * ui_Label20;
extern lv_obj_t * ui_autorepeatertoggle;
extern lv_obj_t * ui_Label18;
extern lv_obj_t * ui_repeatersbutton;
extern lv_obj_t * ui_Label2;
extern lv_obj_t * ui_purgedatabutton;
extern lv_obj_t * ui_Label21;
extern lv_obj_t * ui_renamebox;
extern lv_obj_t * ui_timezonedropdown;
extern lv_obj_t * ui_txpowerslider;
extern lv_obj_t * ui_txpowerlabel;
extern lv_obj_t * ui_Keyboard2;

// Translation
// Orientation toggle
extern lv_obj_t * ui_orientationtoggle;
extern lv_obj_t * ui_orientation_lbl;

// Landscape: button column beside serial monitor (NULL in portrait)
extern lv_obj_t * ui_settings_ser_btncol;


// Stubs for link-compat
extern lv_obj_t * ui_homebutton3;
extern lv_obj_t * ui_homeabel3;
extern lv_obj_t * ui_settingsbutton3;
extern lv_obj_t * ui_settingslabel3;
extern lv_obj_t * ui_backbutton2;
extern lv_obj_t * ui_backlabel2;
extern lv_obj_t * ui_Button3;

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif
