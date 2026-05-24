#pragma once
// ============================================================
// emoji_atlas.h — Noto Color Emoji glyph lookup
// ------------------------------------------------------------
// Holds an in-PSRAM, sorted-by-codepoint array of
// LVGL-compatible image descriptors (LV_IMG_CF_TRUE_COLOR_ALPHA,
// 16-bit color), one per emoji glyph. Backed by two files on the
// SD card:
//   /sdcard/emoji/emoji_atlas_20.bin   — 20×20 glyphs for body text
//   /sdcard/emoji/emoji_atlas_32.bin   — 32×32 glyphs for keyboards / headers
//
// File layout (little-endian, see build_emoji_atlas.py):
//
//   header (16 bytes):
//     magic[4]    = "EMA1"
//     version     uint16 (= 1)
//     glyph_px    uint16 (= 20 or 32)
//     glyph_count uint32
//     reserved    uint32 (= 0)
//
//   codepoints:  glyph_count × uint32 (sorted ascending for bsearch)
//   pixel data:  glyph_count × glyph_px × glyph_px × 3 bytes
//                (RGB565 lo, RGB565 hi, alpha — the native layout for
//                 LV_IMG_CF_TRUE_COLOR_ALPHA with LV_COLOR_DEPTH=16)
//
// If the SD card is missing or the atlas files aren't present, a
// small bundled test atlas (3 solid-color glyphs — yellow 😀,
// red ❤, blue 👍) is used so the pipeline can be visually
// validated before the Python script has been run.
// ============================================================

#include <stdint.h>
#include <stddef.h>
#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

// Which atlas to query.
typedef enum {
    EMOJI_SIZE_SMALL = 0,   // 20×20, used by 12/14-pt body text
    EMOJI_SIZE_BIG   = 1,   // 32×32, used by 16/18/20-pt (headers, keyboard)
    EMOJI_SIZE_COUNT
} emoji_size_t;

// Load both atlases from SD into PSRAM. Returns true if at least
// the bundled fallback is usable — the firmware should never refuse
// to start over a missing atlas. Safe to call before sd_init(), in
// which case only the bundled test atlas is loaded.
bool emoji_atlas_init();

// True if SD-backed atlas for this size loaded successfully (as
// opposed to only the bundled test fallback).
bool emoji_atlas_has_sd(emoji_size_t size);

// Look up an LVGL image descriptor for the given Unicode codepoint.
// Returns NULL if the codepoint isn't in the atlas. The returned
// pointer is valid for the lifetime of the atlas (never freed) and
// the caller must NOT free it.
const lv_img_dsc_t* emoji_atlas_lookup(uint32_t codepoint, emoji_size_t size);

// True if the codepoint has a glyph in EITHER tier. Used by the text
// sanitiser to decide whether to strip an emoji codepoint from a
// display string — no glyph in either tier means LVGL would render
// an empty placeholder box, so the sanitiser drops it instead.
bool emoji_atlas_has(uint32_t codepoint);

// Glyph side length for a given atlas size tier (in pixels).
uint16_t emoji_atlas_glyph_px(emoji_size_t size);

#ifdef __cplusplus
}
#endif
