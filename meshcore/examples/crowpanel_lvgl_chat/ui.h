// ui.h — Master header for programmatic LVGL UI
// Replaces SquareLine-generated version

#ifndef _UI_H
#define _UI_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"
#include "ui_theme.h"
#include "ui_helpers.h"
#include "ui_events.h"

///////////////////// SCREENS ////////////////////

#include "ui_homescreen.h"
#include "ui_settingscreen.h"
#include "ui_repeaterscreen.h"

///////////////////// VARIABLES ////////////////////

extern lv_obj_t * ui____initial_actions0;

// UI INIT
void ui_init(void);
void ui_destroy(void);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif
