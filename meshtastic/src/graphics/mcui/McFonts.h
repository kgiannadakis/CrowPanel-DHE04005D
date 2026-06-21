#pragma once

#if HAS_TFT && USE_MCUI

#include <lvgl.h>

namespace mcui {

const lv_font_t *font_16();

}

#define MCUI_FONT_16 (::mcui::font_16())

#endif
