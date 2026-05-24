// lv_font_greek.h — Greek support: writable font copies with fallback chain
//
// LVGL's built-in Montserrat fonts are `const` (flash on ESP32).
// We can't modify their fallback pointer at runtime.
// Instead, we create writable copies that have Greek fallback set.
//
// Usage:
//   1. Include this header AFTER lvgl.h but BEFORE any font usage
//   2. Call lv_font_greek_init() once after lv_init()
//   3. The macros redirect lv_font_montserrat_XX → writable copies

#ifndef LV_FONT_GREEK_H
#define LV_FONT_GREEK_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Greek supplement fonts (generated from DejaVu Sans)
extern lv_font_t lv_font_greek_10;
extern lv_font_t lv_font_greek_12;
extern lv_font_t lv_font_greek_14;
extern lv_font_t lv_font_greek_16;
extern lv_font_t lv_font_greek_18;
extern lv_font_t lv_font_greek_20;
extern lv_font_t lv_font_greek_22;

// Writable font copies (Montserrat + Greek fallback)
extern lv_font_t lv_font_mg_10;
extern lv_font_t lv_font_mg_12;
extern lv_font_t lv_font_mg_14;
extern lv_font_t lv_font_mg_16;
extern lv_font_t lv_font_mg_18;
extern lv_font_t lv_font_mg_20;
extern lv_font_t lv_font_mg_22;

// Init: copy const Montserrat structs into writable copies, set Greek fallback
void lv_font_greek_init(void);

#ifdef __cplusplus
}
#endif

// ── Redirect all Montserrat references to writable copies ───
// This MUST come after the extern declarations above.
// Every file that includes this header (via ui_theme.h) will use
// the writable copies, which have Greek fallback set.

#define lv_font_montserrat_10  lv_font_mg_10
#define lv_font_montserrat_12  lv_font_mg_12
#define lv_font_montserrat_14  lv_font_mg_14
#define lv_font_montserrat_16  lv_font_mg_16
#define lv_font_montserrat_18  lv_font_mg_18
#define lv_font_montserrat_20  lv_font_mg_20
#define lv_font_montserrat_22  lv_font_mg_22

#endif
