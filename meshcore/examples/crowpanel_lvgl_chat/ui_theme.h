// ui_theme.h — Dark messenger color palette (Signal/Telegram hybrid)
// All screens share this palette. No SquareLine dependency.

#ifndef UI_THEME_H
#define UI_THEME_H

// ── Deep backgrounds ───────────────────────────────────────
#define TH_BG            0x0E1621   // screen background
#define TH_SURFACE       0x17212B   // cards, panels
#define TH_SURFACE2      0x1E2C3A   // elevated cards
#define TH_INPUT         0x242F3D   // text fields
#define TH_BORDER        0x2B3B4D   // subtle borders
#define TH_SEPARATOR     0x1C2A36   // thin lines

// ── Accent / brand ─────────────────────────────────────────
#define TH_ACCENT        0x3390EC   // primary blue (Telegram)
#define TH_ACCENT_LIGHT  0x5EB5F7   // hover / active state
#define TH_GREEN         0x6DC264   // success, delivered
#define TH_RED           0xE05555   // danger, failed
#define TH_AMBER         0xF5A623   // warning, pending

// ── Text ───────────────────────────────────────────────────
#define TH_TEXT          0xF5F5F5   // primary text
#define TH_TEXT2         0x8696A0   // secondary text
#define TH_TEXT3         0x6C7883   // tertiary / placeholder

// ── Chat bubbles ───────────────────────────────────────────
#define TH_BUBBLE_OUT    0x2B5278   // outgoing (deep blue)
#define TH_BUBBLE_IN     0x182533   // incoming (dark slate)

// ── Tab bar ────────────────────────────────────────────────
#define TH_TAB_BG        0x0E1621
#define TH_TAB_ACTIVE    0x3390EC
#define TH_TAB_INACTIVE  0x546E7A

// ── Layout constants (set at boot by init_layout_constants()) ──
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
extern int16_t SCR_W;
extern int16_t SCR_H;
extern int16_t STATUS_H;
extern int16_t TAB_H;
extern int16_t CONTENT_Y;
extern int16_t CONTENT_H;
extern int16_t KB_HEIGHT;
extern int16_t KB_Y_OFFSET;
extern int16_t KB_TA_Y;
extern int16_t TEXTSEND_Y_DEFAULT;
extern int16_t SEARCH_Y_OFFSET;
extern int16_t SETTINGS_KB_TOP;
extern int16_t CHATPANEL_START_Y;
extern int16_t BTN_HALF_W;
#ifdef __cplusplus
}
#endif

// ── Greek character support ────────────────────────────────
#include "lv_font_greek.h"

#endif
