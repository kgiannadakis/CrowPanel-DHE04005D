// ============================================================
// emoji_font.cpp
// ------------------------------------------------------------
// Custom lv_font_t that renders codepoints as color images from
// emoji_atlas. Deliberately does NOT use lv_imgfont, because
// lv_imgfont routes every glyph through a single shared buffer
// (dsc->path[64]) which gives all emojis the same cache key —
// either LVGL's image cache serves stale pixels for the wrong
// emoji, or it has to be disabled entirely (which is what was
// happening during bring-up).
//
// Here, get_glyph_bitmap() returns a pointer directly into the
// atlas's descriptor pool, so every emoji has a unique stable
// pointer. LV_IMG_CACHE_DEF_SIZE > 0 now actually helps:
// previously-rendered emojis reuse their cached decoder session
// on the next draw, skipping the open/close overhead.
//
// bpp = LV_IMGFONT_BPP is the magic cookie that tells LVGL's
// letter-draw path to treat the bitmap as an image source and
// route it through lv_draw_img — same convention lv_imgfont uses.
// ============================================================

#include "emoji_font.h"
#include "emoji_atlas.h"
#include "utils.h"  // serialmon_append

#include <string.h>
#include <stdio.h>
#include <stdint.h>

lv_font_t* g_emoji_font_small = nullptr;
lv_font_t* g_emoji_font_big   = nullptr;

// --- glyph callbacks ----------------------------------------------
//
// LVGL calls get_glyph_dsc once per glyph per layout pass, and
// get_glyph_bitmap once per glyph per draw pass. Both lookups hit
// the same codepoint for the same glyph, so cache the last result
// in a tiny static-slot table — avoids walking the atlas twice for
// every rendered character.
//
// Two separate slot caches, one per tier, because small-tier and
// big-tier can both be in flight for the same frame (e.g. a chat
// header at 20pt next to a body at 14pt).

struct LastHit {
    uint32_t codepoint;       // 0 = invalid
    const lv_img_dsc_t* dsc;
};
static LastHit s_last_small = {0, nullptr};
static LastHit s_last_big   = {0, nullptr};

static inline const lv_img_dsc_t* lookup_cached(uint32_t cp, emoji_size_t tier, LastHit& slot) {
    if (slot.codepoint == cp && slot.dsc) return slot.dsc;
    slot.dsc       = emoji_atlas_lookup(cp, tier);
    slot.codepoint = slot.dsc ? cp : 0;
    return slot.dsc;
}

static bool fill_dsc(lv_font_glyph_dsc_t* out, const lv_img_dsc_t* src) {
    if (!src) return false;
    out->is_placeholder = 0;
    out->adv_w = src->header.w;
    out->box_w = src->header.w;
    out->box_h = src->header.h;
    out->ofs_x = 0;
    out->ofs_y = 0;
    out->bpp   = LV_IMGFONT_BPP;  // tells letter-draw to route through lv_draw_img
    return true;
}

static bool small_get_glyph_dsc(const lv_font_t* /*font*/, lv_font_glyph_dsc_t* dsc_out,
                                uint32_t unicode, uint32_t /*unicode_next*/) {
    return fill_dsc(dsc_out, lookup_cached(unicode, EMOJI_SIZE_SMALL, s_last_small));
}

static const uint8_t* small_get_glyph_bitmap(const lv_font_t* /*font*/, uint32_t unicode) {
    const lv_img_dsc_t* src = lookup_cached(unicode, EMOJI_SIZE_SMALL, s_last_small);
    return (const uint8_t*)src;   // stable pointer into atlas descriptor pool
}

static bool big_get_glyph_dsc(const lv_font_t* /*font*/, lv_font_glyph_dsc_t* dsc_out,
                              uint32_t unicode, uint32_t /*unicode_next*/) {
    return fill_dsc(dsc_out, lookup_cached(unicode, EMOJI_SIZE_BIG, s_last_big));
}

static const uint8_t* big_get_glyph_bitmap(const lv_font_t* /*font*/, uint32_t unicode) {
    const lv_img_dsc_t* src = lookup_cached(unicode, EMOJI_SIZE_BIG, s_last_big);
    return (const uint8_t*)src;
}

// --- font objects -------------------------------------------------

static lv_font_t s_font_small = {};
static lv_font_t s_font_big   = {};

bool emoji_font_init() {
    const uint16_t small_px = emoji_atlas_glyph_px(EMOJI_SIZE_SMALL);
    const uint16_t big_px   = emoji_atlas_glyph_px(EMOJI_SIZE_BIG);

    if (small_px > 0 && !g_emoji_font_small) {
        memset(&s_font_small, 0, sizeof(s_font_small));
        s_font_small.get_glyph_dsc    = small_get_glyph_dsc;
        s_font_small.get_glyph_bitmap = small_get_glyph_bitmap;
        s_font_small.subpx            = LV_FONT_SUBPX_NONE;
        s_font_small.line_height      = small_px;
        s_font_small.base_line        = 0;
        s_font_small.underline_position  = 0;
        s_font_small.underline_thickness = 0;
        g_emoji_font_small = &s_font_small;
        serialmon_append("emoji: small custom font ready");
    }

    if (big_px > 0 && !g_emoji_font_big) {
        memset(&s_font_big, 0, sizeof(s_font_big));
        s_font_big.get_glyph_dsc    = big_get_glyph_dsc;
        s_font_big.get_glyph_bitmap = big_get_glyph_bitmap;
        s_font_big.subpx            = LV_FONT_SUBPX_NONE;
        s_font_big.line_height      = big_px;
        s_font_big.base_line        = 0;
        s_font_big.underline_position  = 0;
        s_font_big.underline_thickness = 0;
        g_emoji_font_big = &s_font_big;
        serialmon_append("emoji: big custom font ready");
    }

    return g_emoji_font_small != nullptr || g_emoji_font_big != nullptr;
}
