#pragma once
// translate.h — Google Translate integration (free unofficial API)

#include <lvgl.h>

void translate_init();
void translate_loop();
void translate_invalidate_bubbles();  // call when chat panel is cleared

// Queue a translation for a live bubble (long-press use case)
void translate_request(const char* text, lv_obj_t* bubble);

// Queue a translation that writes result to chat file + optionally live bubble
void translate_request_to_file(const char* text, const char* chat_key, lv_obj_t* bubble);

// Language helpers
const char* translate_lang_code(int idx);
const char* translate_lang_list();  // newline-separated for lv_dropdown
int translate_lang_count();
