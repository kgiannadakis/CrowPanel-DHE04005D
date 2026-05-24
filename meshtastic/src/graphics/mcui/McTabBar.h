#pragma once
#if HAS_TFT && USE_MCUI
#include <lvgl.h>
namespace mcui {

lv_obj_t *tabbar_create(lv_obj_t *parent);

void tabbar_set_active(int idx);
}
#endif
