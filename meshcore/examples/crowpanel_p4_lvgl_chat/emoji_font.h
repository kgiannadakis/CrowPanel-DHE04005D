#pragma once
// ============================================================
// emoji_font.h — custom lv_font_t over emoji_atlas
// ------------------------------------------------------------
// Builds two lv_font_t instances (small/big) whose glyph
// callbacks hit emoji_atlas directly. Each looked-up emoji
// returns the atlas's own lv_img_dsc_t pointer — stable and
// unique per codepoint — which plays nicely with LVGL's image
// cache (LV_IMG_CACHE_DEF_SIZE in lv_conf.h) so repeated
// renders across frames skip the decoder open/close overhead.
// Deliberately NOT using lv_imgfont, which routes through a
// shared 64-byte buffer and defeats caching. See the comment
// at the top of emoji_font.cpp for the full rationale.
//
// Wired as the emoji-link tail of the Greek → Emoji → Montserrat
// fallback chain — see lv_font_greek_init.c.
// ============================================================

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

// Exposed as lv_font_t* (not lv_font_t) so consumers can store
// them as fallback-pointers. Both are NULL until emoji_font_init()
// succeeds; before that, treat them as "no color emojis".
extern lv_font_t* g_emoji_font_small;   // 20px, for 12–14 pt text
extern lv_font_t* g_emoji_font_big;     // 32px, for 16+ pt text + keyboard

// Build the imgfont objects. Must be called AFTER
// emoji_atlas_init(). Safe to call before the LVGL task is spawned.
bool emoji_font_init();

#ifdef __cplusplus
}
#endif
