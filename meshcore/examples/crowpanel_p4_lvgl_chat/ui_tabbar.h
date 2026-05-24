// ui_tabbar.h — Bottom tab bar shared by all screens

#ifndef UI_TABBAR_H
#define UI_TABBAR_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Create the bottom tab bar on `parent`.
// `active_idx`: 0=Chat, 1=Repeaters, 2=Settings (highlighted tab)
void ui_tabbar_create(lv_obj_t * parent, int active_idx);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif
