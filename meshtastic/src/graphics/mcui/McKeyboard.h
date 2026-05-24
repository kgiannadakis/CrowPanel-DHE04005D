#pragma once
#if HAS_TFT && USE_MCUI
#include <lvgl.h>
namespace mcui {

lv_obj_t *keyboard_create(lv_obj_t *parent);
void keyboard_attach(lv_obj_t *textarea);
void keyboard_show();
void keyboard_hide();
bool keyboard_is_visible();
lv_obj_t *keyboard_get();

}
#endif
