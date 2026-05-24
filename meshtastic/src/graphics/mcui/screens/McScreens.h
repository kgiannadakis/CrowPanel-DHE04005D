#pragma once
#if HAS_TFT && USE_MCUI
#include <lvgl.h>
namespace mcui {

lv_obj_t *chats_screen_create(lv_obj_t *parent);
lv_obj_t *nodes_screen_create(lv_obj_t *parent);
lv_obj_t *maps_screen_create(lv_obj_t *parent);
lv_obj_t *settings_screen_create(lv_obj_t *parent);
void settings_maybe_show_onboarding();

void chats_screen_tick();

void nodes_screen_tick();

void maps_screen_tick();
void maps_storage_prewarm();

void settings_screen_tick();
}
#endif
