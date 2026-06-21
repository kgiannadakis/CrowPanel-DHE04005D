#pragma once
// McEmojiFont - custom LVGL 9.2 lv_font_t whose glyphs are color images from
// McEmojiAtlas. Each looked-up codepoint reports format LV_FONT_GLYPH_FORMAT_IMAGE
// and hands LVGL the atlas's lv_image_dsc_t, which the image draw path renders
// inline with text. Wired as the tail of the chat font fallback chain (McFonts).

#if HAS_TFT && USE_MCUI

#include <lvgl.h>

// NULL until emoji_font_init() succeeds; treat as "no color emoji" before that.
extern lv_font_t *g_emoji_font_small; // 20 px, body/keyboard text
extern lv_font_t *g_emoji_font_big;   // 32 px, headers

// Build the font objects. Must be called AFTER emoji_atlas_init().
bool emoji_font_init(void);

#endif
