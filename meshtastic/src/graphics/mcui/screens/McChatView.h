#pragma once
#if HAS_TFT && USE_MCUI

#include "../data/McMessages.h"
#include <lvgl.h>

namespace mcui {

lv_obj_t *chatview_create(lv_obj_t *parent);

void chatview_open(const McConvId &id, const char *title);

void chatview_hide();

bool chatview_is_open();

void chatview_tick();

McConvId chatview_current();

}

#endif
