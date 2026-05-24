#pragma once
#if HAS_TFT && USE_MCUI
#include <lvgl.h>
namespace mcui {

lv_obj_t *statusbar_create(lv_obj_t *parent);

void statusbar_refresh();

void statusbar_set_visible(bool visible);
}
#endif
