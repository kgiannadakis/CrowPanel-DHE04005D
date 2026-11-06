#pragma once

#if HAS_TFT && USE_MCUI
#include <lvgl.h>

namespace mcui {
void text_edit_register_field(lv_obj_t *textarea);
void text_edit_register_message(lv_obj_t *label);
void text_edit_close();
void text_edit_tick();
bool text_edit_message_selected();
bool text_edit_before_key(lv_obj_t *textarea);
}
#endif
