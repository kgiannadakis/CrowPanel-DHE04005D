// lv_font_greek_init.c — Reversed font chain with color-emoji tail
//
// Chain semantics (primary -> fallback -> fallback ...):
//   lv_font_mg_XX  =  Greek_XX  ->  emoji_color(size-appropriate)  ->  Montserrat_XX
//
// Greek is primary because in some LVGL builds Montserrat has partial
// Greek coverage with wrong glyphs; leaving Montserrat primary broke
// Greek rendering. The emoji slot was previously filled by the
// monochrome Noto Emoji font (lv_font_emoji_16/18); we've replaced
// that with the SD-backed Noto Color Emoji imgfont from
// emoji_font.cpp, so chat bubbles / headers / keyboard buttons now
// render colour emojis inline.
//
// emoji_font_init() must have been called before lv_font_greek_init().
// If it wasn't, g_emoji_font_small / g_emoji_font_big are NULL and
// we just skip the emoji link in the chain -- Greek falls straight
// through to Montserrat and no emojis get drawn.
//
// This file MUST NOT include lv_font_greek.h (it contains macros
// that redirect lv_font_montserrat_XX identifiers).

#include "lvgl.h"
#include "emoji_font.h"
#include <string.h>

// Greek supplement fonts (generated from DejaVu Sans)
extern lv_font_t lv_font_greek_10;
extern lv_font_t lv_font_greek_12;
extern lv_font_t lv_font_greek_14;
extern lv_font_t lv_font_greek_16;
extern lv_font_t lv_font_greek_18;
extern lv_font_t lv_font_greek_20;
extern lv_font_t lv_font_greek_22;

// Writable emoji-font copies. lv_imgfont_create returns an
// lv_font_t* with NO metrics -- we adjust line_height / base_line
// to match the Montserrat size this emoji slot sits between so the
// glyph aligns to the text baseline properly. We copy rather than
// mutate the imgfont directly because the same imgfont instance
// sits in multiple size-chains (one small, one big) and each chain
// wants different metrics.
static lv_font_t s_emoji_12;
static lv_font_t s_emoji_14;
static lv_font_t s_emoji_16;
static lv_font_t s_emoji_18;
static lv_font_t s_emoji_20;

// Writable "mg" fonts -- these become the PRIMARY font used
// everywhere thanks to the macro redirection in lv_font_greek.h.
lv_font_t lv_font_mg_10;
lv_font_t lv_font_mg_12;
lv_font_t lv_font_mg_14;
lv_font_t lv_font_mg_16;
lv_font_t lv_font_mg_18;
lv_font_t lv_font_mg_20;
lv_font_t lv_font_mg_22;

// "Plain" font copies — NO fallback chain. For text-heavy,
// ASCII-only labels (the serial monitor being the main one) where
// Greek + color-emoji support is dead weight. Each character
// render saves 2 font lookups vs the mg_XX chain, which on a
// ~2 KB log label is a measurable redraw speedup. Populated below
// in lv_font_greek_init().
lv_font_t lv_font_plain_12;
lv_font_t lv_font_plain_14;
lv_font_t lv_font_plain_16;

// --- Helpers -----------------------------------------------------

// Copy Montserrat's line metrics onto a font so LVGL lays out mixed
// text without baseline jitter when rendering falls through the
// chain.
static void adopt_metrics(lv_font_t * dst, const lv_font_t * src) {
    dst->line_height          = src->line_height;
    dst->base_line            = src->base_line;
    dst->subpx                = src->subpx;
    dst->underline_position   = src->underline_position;
    dst->underline_thickness  = src->underline_thickness;
}

// Build an emoji link: copy the imgfont, adopt text-size metrics,
// set its fallback to `final`. Returns a pointer to use as the
// fallback of the Greek primary. If the imgfont isn't available
// (atlas init failed / imgfont disabled), returns `final` directly
// so the chain remains Greek -> Montserrat with no emoji middleman.
static lv_font_t * emoji_link(lv_font_t * out, lv_font_t * imgfont,
                              const lv_font_t * final_) {
    if (!imgfont) return (lv_font_t *)final_;
    memcpy(out, imgfont, sizeof(lv_font_t));
    adopt_metrics(out, final_);
    out->fallback = (lv_font_t *)final_;
    return out;
}

// Assemble the final chain for one size.
// Greek gets its line metrics set to Montserrat's so multi-font
// labels layout consistently; its fallback points at the
// emoji-link (which itself falls through to Montserrat).
static void setup_font(lv_font_t * out, lv_font_t * greek, lv_font_t * emoji_or_final) {
    adopt_metrics(greek, emoji_or_final);  // mirror metrics of the next link
    greek->fallback = emoji_or_final;
    memcpy(out, greek, sizeof(lv_font_t));
}

void lv_font_greek_init(void) {
    // Which imgfont size covers each text size:
    //   SMALL (20px) for 12-18 pt text — body chat, headers, keyboard
    //     buttons. 20 px is roughly the cap height of 18 pt Montserrat,
    //     so emojis look about the same visual height as the letters
    //     next to them, not 2x the text as with 32 px.
    //   BIG (32px) reserved for 20 pt+ where the emoji is the focal
    //     element (e.g. large title glyphs). Not currently used in
    //     chat UI but kept in the chain for future headers.
    lv_font_t * const imgfont_small = g_emoji_font_small;
    lv_font_t * const imgfont_big   = g_emoji_font_big;

    // 10 pt: Greek -> Montserrat (no emoji; too small to render usefully)
    setup_font(&lv_font_mg_10, &lv_font_greek_10, (lv_font_t *)&lv_font_montserrat_10);

    // 12 pt: Greek -> emoji(small) -> Montserrat
    setup_font(&lv_font_mg_12, &lv_font_greek_12,
               emoji_link(&s_emoji_12, imgfont_small, &lv_font_montserrat_12));

    // 14 pt: Greek -> emoji(small) -> Montserrat  (keyboard default)
    setup_font(&lv_font_mg_14, &lv_font_greek_14,
               emoji_link(&s_emoji_14, imgfont_small, &lv_font_montserrat_14));

    // 16 pt: Greek -> emoji(small) -> Montserrat  (chat body)
    setup_font(&lv_font_mg_16, &lv_font_greek_16,
               emoji_link(&s_emoji_16, imgfont_small, &lv_font_montserrat_16));

    // 18 pt: Greek -> emoji(small) -> Montserrat  (section headers)
    setup_font(&lv_font_mg_18, &lv_font_greek_18,
               emoji_link(&s_emoji_18, imgfont_small, &lv_font_montserrat_18));

    // 20 pt (chat header title): Greek -> emoji(big) -> Montserrat
    setup_font(&lv_font_mg_20, &lv_font_greek_20,
               emoji_link(&s_emoji_20, imgfont_big, &lv_font_montserrat_20));

    // 22 pt: Greek -> Montserrat (rarely used; skip emoji to keep metrics clean)
    setup_font(&lv_font_mg_22, &lv_font_greek_22, (lv_font_t *)&lv_font_montserrat_22);

    // Plain (no-fallback) copies of raw Montserrat for text-heavy
    // ASCII-only widgets. This file does NOT include lv_font_greek.h,
    // so `lv_font_montserrat_XX` here references the ORIGINAL const
    // LVGL struct (not our `lv_font_mg_XX` wrapper). Clearing
    // fallback = NULL means get_glyph_dsc returns after ONE lookup
    // instead of walking three fonts per character.
    memcpy(&lv_font_plain_12, &lv_font_montserrat_12, sizeof(lv_font_t));
    lv_font_plain_12.fallback = NULL;
    memcpy(&lv_font_plain_14, &lv_font_montserrat_14, sizeof(lv_font_t));
    lv_font_plain_14.fallback = NULL;
    memcpy(&lv_font_plain_16, &lv_font_montserrat_16, sizeof(lv_font_t));
    lv_font_plain_16.fallback = NULL;
}
