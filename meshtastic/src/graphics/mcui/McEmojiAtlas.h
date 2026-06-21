#pragma once
// McEmojiAtlas - Noto Color Emoji glyph lookup for the mcui chat UI.
//
// Loads, once per size tier, a PSRAM array of LVGL 9.2 lv_image_dsc_t (one per
// emoji glyph, LV_COLOR_FORMAT_RGB565A8). Backed by two files on the SD card:
//   /sdcard/emoji/emoji_atlas_20.bin   - 20x20 glyphs for body text
//   /sdcard/emoji/emoji_atlas_32.bin   - 32x32 glyphs for headers
//
// On-disk format (little-endian, from meshcore tools/build_emoji_atlas.py):
//   header(16): magic "EMA1", u16 version(=1), u16 glyph_px, u32 glyph_count, u32 reserved
//   codepoints: glyph_count * u32 (sorted ascending)
//   pixels:     glyph_count * glyph_px*glyph_px * 3 bytes, INTERLEAVED per pixel
//               (RGB565 lo, RGB565 hi, alpha). This is the v8 TRUE_COLOR_ALPHA
//               layout; we de-interleave each glyph into the planar
//               [RGB565 plane][A8 plane] layout LVGL 9.2 RGB565A8 expects.
//
// If the SD card / atlas files are missing, a tiny bundled 3-glyph test atlas
// is used so the firmware never refuses to boot over a missing atlas.

#if HAS_TFT && USE_MCUI

#include <lvgl.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    EMOJI_SIZE_SMALL = 0, // 20x20, body text
    EMOJI_SIZE_BIG = 1,   // 32x32, headers
    EMOJI_SIZE_COUNT
} emoji_size_t;

// Load both atlases (mounts the SD card briefly, then unmounts it). Safe to call
// once, early in boot. Returns true if at least the bundled fallback is usable.
bool emoji_atlas_init(void);

// LVGL 9.2 image descriptor for the codepoint, or NULL if absent. The pointer is
// valid for the lifetime of the atlas (never freed); the caller must not free it.
const lv_image_dsc_t *emoji_atlas_lookup(uint32_t codepoint, emoji_size_t size);

// True if the codepoint has a glyph in either tier.
bool emoji_atlas_has(uint32_t codepoint);

// Glyph side length (px) for a tier, or 0 if that tier failed to load.
uint16_t emoji_atlas_glyph_px(emoji_size_t size);

#endif
