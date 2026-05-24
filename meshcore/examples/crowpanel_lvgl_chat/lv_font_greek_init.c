// lv_font_greek_init.c — Reversed font chain: Greek primary, Montserrat fallback
//
// Problem: Montserrat in some LVGL builds has partial Greek codepoint coverage
// with wrong glyph data. Since Montserrat was primary, our correct Greek fallback
// was never reached for those codepoints.
//
// Fix: Make Greek the PRIMARY font. Montserrat becomes the FALLBACK.
// - Greek chars → found in primary → correct rendering
// - Latin chars → not found in primary → fallback to Montserrat → correct rendering
//
// This file MUST NOT include lv_font_greek.h (contains macros that
// redirect lv_font_montserrat_XX names).

#include "lvgl.h"
#include <string.h>

// Greek supplement fonts (generated from DejaVu Sans)
extern lv_font_t lv_font_greek_10;
extern lv_font_t lv_font_greek_12;
extern lv_font_t lv_font_greek_14;
extern lv_font_t lv_font_greek_16;
extern lv_font_t lv_font_greek_18;
extern lv_font_t lv_font_greek_20;
extern lv_font_t lv_font_greek_22;


// Emoji fonts (Noto Emoji monochrome)
extern const lv_font_t lv_font_emoji_16;
extern const lv_font_t lv_font_emoji_18;
static lv_font_t s_emoji_16_writable;
static lv_font_t s_emoji_18_writable;

// Writable font copies — these become the "combined" fonts
lv_font_t lv_font_mg_10;
lv_font_t lv_font_mg_12;
lv_font_t lv_font_mg_14;
lv_font_t lv_font_mg_16;
lv_font_t lv_font_mg_18;
lv_font_t lv_font_mg_20;
lv_font_t lv_font_mg_22;

// Helper: set up one font size with reversed chain
static void setup_font(lv_font_t * out, lv_font_t * greek, const lv_font_t * montserrat) {
    // Copy Montserrat's line metrics to Greek font for consistent text layout
    greek->line_height = montserrat->line_height;
    greek->base_line   = montserrat->base_line;
    greek->subpx       = montserrat->subpx;
    greek->underline_position  = montserrat->underline_position;
    greek->underline_thickness = montserrat->underline_thickness;

    // Set Montserrat as fallback on Greek
    greek->fallback = (lv_font_t *)montserrat;

    // The writable copy IS the Greek font (with Montserrat fallback)
    memcpy(out, greek, sizeof(lv_font_t));
}

void lv_font_greek_init(void) {
    setup_font(&lv_font_mg_10, &lv_font_greek_10, &lv_font_montserrat_10);
    setup_font(&lv_font_mg_12, &lv_font_greek_12, &lv_font_montserrat_12);
    setup_font(&lv_font_mg_14, &lv_font_greek_14, &lv_font_montserrat_14);
    // Size 16: insert emoji into chain — Greek → Emoji → Montserrat
    // 1. Make writable emoji copy with Montserrat as its fallback
    memcpy(&s_emoji_16_writable, &lv_font_emoji_16, sizeof(lv_font_t));
    s_emoji_16_writable.line_height          = lv_font_montserrat_16.line_height;
    s_emoji_16_writable.base_line            = lv_font_montserrat_16.base_line;
    s_emoji_16_writable.subpx                = lv_font_montserrat_16.subpx;
    s_emoji_16_writable.underline_position   = lv_font_montserrat_16.underline_position;
    s_emoji_16_writable.underline_thickness  = lv_font_montserrat_16.underline_thickness;
    s_emoji_16_writable.fallback = (lv_font_t *)&lv_font_montserrat_16;
    // 2. Set up Greek → Emoji (which falls back to Montserrat)
    setup_font(&lv_font_mg_16, &lv_font_greek_16, &s_emoji_16_writable);

    // Size 18: insert emoji into chain — Greek → Emoji → Montserrat
    memcpy(&s_emoji_18_writable, &lv_font_emoji_18, sizeof(lv_font_t));
    s_emoji_18_writable.line_height          = lv_font_montserrat_18.line_height;
    s_emoji_18_writable.base_line            = lv_font_montserrat_18.base_line;
    s_emoji_18_writable.subpx                = lv_font_montserrat_18.subpx;
    s_emoji_18_writable.underline_position   = lv_font_montserrat_18.underline_position;
    s_emoji_18_writable.underline_thickness  = lv_font_montserrat_18.underline_thickness;
    s_emoji_18_writable.fallback = (lv_font_t *)&lv_font_montserrat_18;
    setup_font(&lv_font_mg_18, &lv_font_greek_18, &s_emoji_18_writable);
    setup_font(&lv_font_mg_20, &lv_font_greek_20, &lv_font_montserrat_20);
    setup_font(&lv_font_mg_22, &lv_font_greek_22, &lv_font_montserrat_22);
}
