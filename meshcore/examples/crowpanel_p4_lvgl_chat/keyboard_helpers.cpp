// ============================================================
// keyboard_helpers.cpp — v11 keyboard maps + layout constants + the
// kb_show/kb_hide/setup_keyboard helpers. Originally lived in v11's
// display.cpp alongside the LovyanGFX driver init; on P4 the panel is
// driven directly by display.cpp, so we only keep the LVGL-level bits.
// ============================================================

#include "utils.h"
#include "chat_ui.h"
#include "home_ui.h"
#include "settings_cb.h"
#include "repeater_ui.h"
#include "features_ui.h"
#include "app_globals.h"
#include "lv_font_greek.h"

#include <extra/widgets/keyboard/lv_keyboard.h>

#include "persistence.h"

#include "ui.h"
#include "ui_homescreen.h"
#include "ui_settingscreen.h"
#include "ui_repeaterscreen.h"

// ---- Layout globals (set once at boot) ----
extern "C" {
  int16_t SCR_W              = 480;
  int16_t SCR_H              = 800;
  int16_t STATUS_H           = 50;
  int16_t TAB_H              = 60;
  int16_t CONTENT_Y          = 50;
  int16_t CONTENT_H          = 690;
  int16_t KB_HEIGHT          = 280;
  int16_t KB_Y_OFFSET        = 520;
  int16_t KB_TA_Y            = 470;
  int16_t TEXTSEND_Y_DEFAULT = 694;
  int16_t SEARCH_Y_OFFSET    = -324;
  int16_t SETTINGS_KB_TOP    = 520;
  int16_t CHATPANEL_START_Y  = 100;
  int16_t BTN_HALF_W         = 215;
}
// Default to portrait; main.cpp overrides this from NVS before ui_init().
// UI can render portrait (480×800) on the hardwired 800×480 landscape RGB
// panel. Default to portrait — it's the orientation the v11 SquareLine UI
// was originally authored for, so every screen lays out cleanly without
// extra landscape branches.
bool g_landscape_mode = false;

void init_layout_constants() {
  if (g_landscape_mode) {
    // 792×479 — trimmed 8 px off the left and 1 row off the top so
    // the pioarduino/IDF 5.5.4 RGB driver's fixed -8/-1 scan-start
    // offset maps the untouched FB right-strip + bottom-row onto
    // panel[0..7, *] and panel[*, 0] as black instead of wrapping
    // the right-edge UI content. See the header comment in
    // display.cpp for the full explanation.
    SCR_W = 792;  SCR_H = 479;
    STATUS_H = 40; TAB_H = 50;
    CONTENT_Y = STATUS_H;
    CONTENT_H = SCR_H - STATUS_H - TAB_H;   // 389
    KB_HEIGHT = 190;  KB_Y_OFFSET = SCR_H - KB_HEIGHT;
    KB_TA_Y = KB_Y_OFFSET - 50;
    // Text field idle Y — sit the field right above the tab bar with
    // just a 2 px breathing gap. The ui_textsendtype widget height is
    // 44 (see ui_homescreen.c), so (SCR_H - TAB_H - 46) puts the
    // field's bottom edge 2 px above the tab bar top.
    TEXTSEND_Y_DEFAULT = SCR_H - TAB_H - 46;
    SEARCH_Y_OFFSET    = -178;
    SETTINGS_KB_TOP    = KB_Y_OFFSET;
    CHATPANEL_START_Y  = 80;
    BTN_HALF_W         = 376;
  } else {
    SCR_W = 480;  SCR_H = 800;
    STATUS_H = 50; TAB_H = 60;
    CONTENT_Y = STATUS_H;
    CONTENT_H = SCR_H - STATUS_H - TAB_H;   // 690
    KB_HEIGHT = 280;  KB_Y_OFFSET = SCR_H - KB_HEIGHT;
    KB_TA_Y = KB_Y_OFFSET - 50;
    // See landscape branch — 46 = textfield_height(44) + 2 px gap.
    TEXTSEND_Y_DEFAULT = SCR_H - TAB_H - 46;
    SEARCH_Y_OFFSET    = -324;
    SETTINGS_KB_TOP    = KB_Y_OFFSET;
    CHATPANEL_START_Y  = 100;
    BTN_HALF_W         = 215;
  }
}

// ---- Display driver callbacks ----
// Display buffers, GT911 input, gestures and touch wake are owned by
// display.cpp. This module owns keyboard maps and keyboard visibility.

// ---- Custom keyboard maps ----
#define _KB_N    (LV_BTNMATRIX_CTRL_NO_REPEAT | LV_BTNMATRIX_CTRL_POPOVER | 1)
#define _KB_LC   (LV_BTNMATRIX_CTRL_NO_REPEAT | LV_BTNMATRIX_CTRL_POPOVER | 4)
#define _KB_SM   (LV_BTNMATRIX_CTRL_NO_REPEAT | LV_BTNMATRIX_CTRL_POPOVER | 3)
#define _KB_XS   (LV_BTNMATRIX_CTRL_NO_REPEAT | LV_BTNMATRIX_CTRL_POPOVER | 1)
#define _KB_CTRL (LV_BTNMATRIX_CTRL_NO_REPEAT | LV_BTNMATRIX_CTRL_CLICK_TRIG | LV_BTNMATRIX_CTRL_CHECKED)
#define _KB_SHFT (LV_BTNMATRIX_CTRL_NO_REPEAT | LV_BTNMATRIX_CTRL_CHECKABLE | 1)
#define _KB_EN (LV_BTNMATRIX_CTRL_NO_REPEAT | LV_BTNMATRIX_CTRL_POPOVER | LV_BTNMATRIX_CTRL_CLICK_TRIG | 1)

enum {
  KB_LANG_EN = 0,
  KB_LANG_EL = 1,
  KB_LANG_DE = 2
};

static void kb_set_language(uint8_t lang) {
  g_kb_lang = lang;
  g_kb_greek = (lang == KB_LANG_EL);
}

#define KB_DE_A_LC "\xC3\xA4"
#define KB_DE_O_LC "\xC3\xB6"
#define KB_DE_U_LC "\xC3\xBC"
#define KB_DE_A_UC "\xC3\x84"
#define KB_DE_O_UC "\xC3\x96"
#define KB_DE_U_UC "\xC3\x9C"

static const char * const kb_en_lc[] = {
    "1","2","3","4","5","6","7","8","9","0", "\n",
    "q","w","e","r","t","y","u","i","o","p", "\n",
    "a","s","d","f","g","h","j","k","l", "\n",
    "z","x","c","v","b","n","m",".", LV_SYMBOL_BACKSPACE, "\n",
    "Aa","!","EL","1#"," ","?", LV_SYMBOL_OK, ""
};
static const char * const kb_en_uc[] = {
    "1","2","3","4","5","6","7","8","9","0", "\n",
    "Q","W","E","R","T","Y","U","I","O","P", "\n",
    "A","S","D","F","G","H","J","K","L", "\n",
    "Z","X","C","V","B","N","M",".", LV_SYMBOL_BACKSPACE, "\n",
    "Aa","!","EL","1#"," ","?", LV_SYMBOL_OK, ""
};
static const char * const kb_de_lc[] = {
    "1","2","3","4","5","6","7","8","9","0", "\n",
    "q","w","e","r","t","z","u","i","o","p",KB_DE_U_LC, "\n",
    "a","s","d","f","g","h","j","k","l",KB_DE_O_LC,KB_DE_A_LC, "\n",
    "y","x","c","v","b","n","m", LV_SYMBOL_BACKSPACE, "\n",
    "Aa","!","EN","1#"," ","?", LV_SYMBOL_OK, ""
};
static const char * const kb_de_uc[] = {
    "1","2","3","4","5","6","7","8","9","0", "\n",
    "Q","W","E","R","T","Z","U","I","O","P",KB_DE_U_UC, "\n",
    "A","S","D","F","G","H","J","K","L",KB_DE_O_UC,KB_DE_A_UC, "\n",
    "Y","X","C","V","B","N","M", LV_SYMBOL_BACKSPACE, "\n",
    "Aa","!","EN","1#"," ","?", LV_SYMBOL_OK, ""
};
static const char * const kb_gr_lc[] = {
    "1","2","3","4","5","6","7","8","9","0", LV_SYMBOL_BACKSPACE, "\n",
    ";","\xCF\x82","\xCE\xB5","\xCF\x81","\xCF\x84","\xCF\x85","\xCE\xB8","\xCE\xB9","\xCE\xBF","\xCF\x80", "\n",
    "\xCE\xB1","\xCF\x83","\xCE\xB4","\xCF\x86","\xCE\xB3","\xCE\xB7","\xCE\xBE","\xCE\xBA","\xCE\xBB", LV_SYMBOL_NEW_LINE, "\n",
    "\xCE\xB6","\xCF\x87","\xCF\x88","\xCF\x89","\xCE\xB2","\xCE\xBD","\xCE\xBC",".",",","!", "\n",
    "Aa","!","DE","1#"," ","?", LV_SYMBOL_OK, ""
};
static const char * const kb_gr_uc[] = {
    "1","2","3","4","5","6","7","8","9","0", LV_SYMBOL_BACKSPACE, "\n",
    ":","\xCE\xA3","\xCE\x95","\xCE\xA1","\xCE\xA4","\xCE\xA5","\xCE\x98","\xCE\x99","\xCE\x9F","\xCE\xA0", "\n",
    "\xCE\x91","\xCE\xA3","\xCE\x94","\xCE\xA6","\xCE\x93","\xCE\x97","\xCE\x9E","\xCE\x9A","\xCE\x9B", LV_SYMBOL_NEW_LINE, "\n",
    "\xCE\x96","\xCE\xA7","\xCE\xA8","\xCE\xA9","\xCE\x92","\xCE\x9D","\xCE\x9C",".",",","!", "\n",
    "Aa","!","DE","1#"," ","?", LV_SYMBOL_OK, ""
};

static const lv_btnmatrix_ctrl_t kb_ctrl_en_lc[] = {
    _KB_EN,_KB_EN,_KB_EN,_KB_EN,_KB_EN,_KB_EN,_KB_EN,_KB_EN,_KB_EN,_KB_EN,
    _KB_EN,_KB_EN,_KB_EN,_KB_EN,_KB_EN,_KB_EN,_KB_EN,_KB_EN,_KB_EN,_KB_EN,
    _KB_EN,_KB_EN,_KB_EN,_KB_EN,_KB_EN,_KB_EN,_KB_EN,_KB_EN,_KB_EN,
    _KB_EN|4,_KB_EN|4,_KB_EN|4,_KB_EN|4,_KB_EN|4,_KB_EN|4,_KB_EN|4,_KB_EN|2, LV_BTNMATRIX_CTRL_CHECKED|6,
    _KB_SHFT, _KB_N, _KB_CTRL, _KB_CTRL, LV_BTNMATRIX_CTRL_NO_REPEAT|5, _KB_N, _KB_CTRL|2
};
static const lv_btnmatrix_ctrl_t kb_ctrl_en_uc[] = {
    _KB_EN,_KB_EN,_KB_EN,_KB_EN,_KB_EN,_KB_EN,_KB_EN,_KB_EN,_KB_EN,_KB_EN,
    _KB_EN,_KB_EN,_KB_EN,_KB_EN,_KB_EN,_KB_EN,_KB_EN,_KB_EN,_KB_EN,_KB_EN,
    _KB_EN,_KB_EN,_KB_EN,_KB_EN,_KB_EN,_KB_EN,_KB_EN,_KB_EN,_KB_EN,
    _KB_EN|4,_KB_EN|4,_KB_EN|4,_KB_EN|4,_KB_EN|4,_KB_EN|4,_KB_EN|4,_KB_EN|2, LV_BTNMATRIX_CTRL_CHECKED|6,
    _KB_SHFT|LV_BTNMATRIX_CTRL_CHECKED, _KB_N, _KB_CTRL, _KB_CTRL, LV_BTNMATRIX_CTRL_NO_REPEAT|5, _KB_N, _KB_CTRL|2
};

static const lv_btnmatrix_ctrl_t kb_ctrl_lc[] = {
    _KB_N,_KB_N,_KB_N,_KB_N,_KB_N,_KB_N,_KB_N,_KB_N,_KB_N,_KB_N, LV_BTNMATRIX_CTRL_CHECKED|2,
    _KB_LC,_KB_LC,_KB_LC,_KB_LC,_KB_LC,_KB_LC,_KB_LC,_KB_LC,_KB_LC,_KB_LC,
    _KB_SM,_KB_SM,_KB_SM,_KB_SM,_KB_SM,_KB_SM,_KB_SM,_KB_SM,_KB_SM, LV_BTNMATRIX_CTRL_CHECKED|7,
    _KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,
    _KB_SHFT, _KB_N, _KB_CTRL, _KB_CTRL, LV_BTNMATRIX_CTRL_NO_REPEAT|5, _KB_N, _KB_CTRL|2
};
static const lv_btnmatrix_ctrl_t kb_ctrl_uc[] = {
    _KB_N,_KB_N,_KB_N,_KB_N,_KB_N,_KB_N,_KB_N,_KB_N,_KB_N,_KB_N, LV_BTNMATRIX_CTRL_CHECKED|2,
    _KB_LC,_KB_LC,_KB_LC,_KB_LC,_KB_LC,_KB_LC,_KB_LC,_KB_LC,_KB_LC,_KB_LC,
    _KB_SM,_KB_SM,_KB_SM,_KB_SM,_KB_SM,_KB_SM,_KB_SM,_KB_SM,_KB_SM, LV_BTNMATRIX_CTRL_CHECKED|7,
    _KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,
    _KB_SHFT|LV_BTNMATRIX_CTRL_CHECKED, _KB_N, _KB_CTRL, _KB_CTRL, LV_BTNMATRIX_CTRL_NO_REPEAT|5, _KB_N, _KB_CTRL|2
};
static const lv_btnmatrix_ctrl_t kb_ctrl_de_lc[] = {
    _KB_N,_KB_N,_KB_N,_KB_N,_KB_N,_KB_N,_KB_N,_KB_N,_KB_N,_KB_N,
    _KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,
    _KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,
    _KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS, LV_BTNMATRIX_CTRL_CHECKED|2,
    _KB_SHFT, _KB_N, _KB_CTRL, _KB_CTRL, LV_BTNMATRIX_CTRL_NO_REPEAT|5, _KB_N, _KB_CTRL|2
};
static const lv_btnmatrix_ctrl_t kb_ctrl_de_uc[] = {
    _KB_N,_KB_N,_KB_N,_KB_N,_KB_N,_KB_N,_KB_N,_KB_N,_KB_N,_KB_N,
    _KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,
    _KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,
    _KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS, LV_BTNMATRIX_CTRL_CHECKED|2,
    _KB_SHFT|LV_BTNMATRIX_CTRL_CHECKED, _KB_N, _KB_CTRL, _KB_CTRL, LV_BTNMATRIX_CTRL_NO_REPEAT|5, _KB_N, _KB_CTRL|2
};

// Five rows contribute four newline entries and one terminating empty string.
#define KB_CHECK_MAP(map, ctrl) static_assert(sizeof(map) / sizeof((map)[0]) == sizeof(ctrl) / sizeof((ctrl)[0]) + 5, "Keyboard map/control mismatch")
KB_CHECK_MAP(kb_en_lc, kb_ctrl_en_lc);
KB_CHECK_MAP(kb_en_uc, kb_ctrl_en_uc);
KB_CHECK_MAP(kb_gr_lc, kb_ctrl_lc);
KB_CHECK_MAP(kb_gr_uc, kb_ctrl_uc);
KB_CHECK_MAP(kb_de_lc, kb_ctrl_de_lc);
KB_CHECK_MAP(kb_de_uc, kb_ctrl_de_uc);
#undef KB_CHECK_MAP

static const char * const custom_kb_map_sym[] = {
    "1","2","3","4","5","6","7","8","9","0", LV_SYMBOL_BACKSPACE, "\n",
    "+","-","*","/","=","%","!","?","@","#", "\n",
    "(",")","{","}","[","]","\\",";","\"","'", LV_SYMBOL_NEW_LINE, "\n",
    "_","~","<",">","$","^","&",".","," ,":", "\n",
    "abc","Emoji"," ", "?", LV_SYMBOL_OK, ""
};
static const lv_btnmatrix_ctrl_t custom_kb_ctrl_sym[] = {
    _KB_N,_KB_N,_KB_N,_KB_N,_KB_N,_KB_N,_KB_N,_KB_N,_KB_N,_KB_N, LV_BTNMATRIX_CTRL_CHECKED|2,
    _KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,
    _KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS, LV_BTNMATRIX_CTRL_CHECKED|7,
    _KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,
    _KB_CTRL|2, _KB_CTRL|2, 4, LV_BTNMATRIX_CTRL_CHECKED|2, _KB_CTRL|2
};

// Emoji keyboard pages (using LV_KEYBOARD_MODE_NUMBER)
// Page 1: faces
static const char * const kb_emoji_1[] = {
    "\xF0\x9F\x98\x80","\xF0\x9F\x98\x83","\xF0\x9F\x98\x84","\xF0\x9F\x98\x81","\xF0\x9F\x98\x86","\xF0\x9F\x98\x85","\xF0\x9F\x98\x82","\xF0\x9F\xA4\xA3","\xF0\x9F\x98\x8A","\xF0\x9F\x98\x87", LV_SYMBOL_BACKSPACE, "\n",
    "\xF0\x9F\x98\x8D","\xF0\x9F\xA5\xB0","\xF0\x9F\x98\x98","\xF0\x9F\x98\x9A","\xF0\x9F\x98\x8B","\xF0\x9F\x98\x9B","\xF0\x9F\x98\x9C","\xF0\x9F\xA4\xAA","\xF0\x9F\x98\x9D","\xF0\x9F\xA4\x91", "\n",
    "\xF0\x9F\xA4\x97","\xF0\x9F\xA4\x94","\xF0\x9F\x98\x8E","\xF0\x9F\xA4\xA9","\xF0\x9F\x98\x8F","\xF0\x9F\x98\x92","\xF0\x9F\x98\x9E","\xF0\x9F\x98\xA2","\xF0\x9F\x98\xAD","\xF0\x9F\x98\xA4", "\n",
    "\xF0\x9F\x98\xA1","\xF0\x9F\xA4\xAC","\xF0\x9F\x98\xB1","\xF0\x9F\x98\xB0","\xF0\x9F\xA5\xBA","\xF0\x9F\x98\xB4","\xF0\x9F\x92\xA9","\xF0\x9F\x92\x80","\xF0\x9F\x91\xBB","\xF0\x9F\x91\xBD", "\n",
    "abc","\xE2\x96\xB6"," ", LV_SYMBOL_OK, ""
};
// 😀😃😄😁😆😅😂🤣😊😇 ⌫
// 😍🥰😘😚😋😛😜🤪😝🤑
// 🤗🤔😎🤩😏😒😞😢😭😤
// 😡🤬😱😰🥺😴💩💀👻👽
// abc ▶ [space] ✓

// Page 2: hands, hearts, objects
static const char * const kb_emoji_2[] = {
    "\xF0\x9F\x91\x8D","\xF0\x9F\x91\x8E","\xF0\x9F\x91\x8F","\xF0\x9F\x99\x8C","\xF0\x9F\x91\x8A","\xE2\x9C\x8A","\xE2\x9C\x8C","\xF0\x9F\xA4\x9E","\xE2\x9C\x8B","\xF0\x9F\x99\x8F", LV_SYMBOL_BACKSPACE, "\n",
    "\xE2\x9D\xA4","\xF0\x9F\xA7\xA1","\xF0\x9F\x92\x9B","\xF0\x9F\x92\x9A","\xF0\x9F\x92\x99","\xF0\x9F\x92\x9C","\xF0\x9F\x96\xA4","\xF0\x9F\x92\x94","\xF0\x9F\x94\xA5","\xF0\x9F\x92\xAF", "\n",
    "\xE2\x9C\x85","\xE2\x9C\xA8","\xE2\xAD\x90","\xF0\x9F\x8C\x9F","\xF0\x9F\x8E\x89","\xF0\x9F\x8E\x81","\xF0\x9F\x8F\x86","\xF0\x9F\x92\xB0","\xF0\x9F\x93\xB1","\xF0\x9F\x92\xBB", "\n",
    "\xF0\x9F\x8D\xBB","\xF0\x9F\x8D\xBA","\xF0\x9F\x8D\xBD","\xF0\x9F\x8D\xBE","\xE2\x98\x80","\xE2\x9B\x85","\xE2\x9B\x84","\xE2\x9B\xBD","\xE2\x9B\xB5","\xE2\x9B\xAA", "\n",
    "abc","\xE2\x97\x80"," ", LV_SYMBOL_OK, ""
};
// 👍👎👏🙌👊✊✌🤞✋🙏 ⌫
// ❤🧡💛💚💙💜🖤💔🔥💯
// ✅✨⭐🌟🎉🎁🏆💰📱💻
// 🍻🍺🍽🍾☀⛅⛄⛽⛵⛪
// abc ◀ [space] ✓

static int s_emoji_page = 0;

static const lv_btnmatrix_ctrl_t kb_ctrl_emoji[] = {
    _KB_LC,_KB_LC,_KB_LC,_KB_LC,_KB_LC,_KB_LC,_KB_LC,_KB_LC,_KB_LC,_KB_LC, LV_BTNMATRIX_CTRL_CHECKED|2,
    _KB_LC,_KB_LC,_KB_LC,_KB_LC,_KB_LC,_KB_LC,_KB_LC,_KB_LC,_KB_LC,_KB_LC,
    _KB_LC,_KB_LC,_KB_LC,_KB_LC,_KB_LC,_KB_LC,_KB_LC,_KB_LC,_KB_LC,_KB_LC,
    _KB_LC,_KB_LC,_KB_LC,_KB_LC,_KB_LC,_KB_LC,_KB_LC,_KB_LC,_KB_LC,_KB_LC,
    _KB_CTRL|2, _KB_CTRL|2, 4, _KB_CTRL|2
};

// ---- Keyboard functions ----
// Session-only clipboard. Four bytes per Unicode character plus terminator.
static char s_clipboard[1025] = {};
static lv_obj_t* s_clipboard_menu = nullptr;
static lv_obj_t* s_clipboard_target = nullptr;
static lv_obj_t* s_selection_handles[2] = {};
static uint32_t s_selection_start = 0;
static uint32_t s_selection_end = 0;
static lv_point_t s_handle_grab;
static lv_point_t s_handle_anchor;

bool kb_edit_touch_contains(int x, int y) {
  lv_obj_t* objects[] = {s_clipboard_target, s_clipboard_menu,
                         s_selection_handles[0], s_selection_handles[1]};
  for (lv_obj_t* obj : objects) {
    if (!obj || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) continue;
    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    const int margin = (obj == s_selection_handles[0] || obj == s_selection_handles[1]) ? 8 : 0;
    if (x >= area.x1 - margin && x <= area.x2 + margin &&
        y >= area.y1 - margin && y <= area.y2 + margin) return true;
  }
  return false;
}

static void clipboard_close() {
  if (s_clipboard_target) lv_textarea_clear_selection(s_clipboard_target);
  for (lv_obj_t*& handle : s_selection_handles) {
    if (handle) lv_obj_del(handle);
    handle = nullptr;
  }
  if (s_clipboard_menu) lv_obj_del(s_clipboard_menu);
  s_clipboard_menu = nullptr;
  s_clipboard_target = nullptr;
}

static void selection_update() {
  if (!s_clipboard_target) return;
  lv_obj_t* label = lv_textarea_get_label(s_clipboard_target);
  lv_obj_set_style_bg_color(label, lv_color_hex(0x2196F3), LV_PART_SELECTED);
  lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_SELECTED);
  lv_label_set_text_sel_start(label, s_selection_start);
  lv_label_set_text_sel_end(label, s_selection_end);
  lv_area_t label_area, field_area;
  lv_obj_get_coords(label, &label_area);
  lv_obj_get_coords(s_clipboard_target, &field_area);
  const lv_font_t* font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
  for (unsigned i = 0; i < 2; ++i) {
    lv_point_t pos;
    lv_label_get_letter_pos(label, i ? s_selection_end : s_selection_start, &pos);
    const lv_coord_t x = label_area.x1 + pos.x;
    const lv_coord_t y = label_area.y1 + pos.y + font->line_height;
    lv_obj_t* handle = s_selection_handles[i];
    if (!handle) continue;
    if (s_selection_start == s_selection_end || x < field_area.x1 || x > field_area.x2 ||
        y < field_area.y1 || y > field_area.y2 + font->line_height) {
      lv_obj_add_flag(handle, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_clear_flag(handle, LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_pos(handle, LV_CLAMP(0, x - 14, SCR_W - 28), LV_CLAMP(0, y - 5, SCR_H - 28));
    }
  }
}

static void selection_handle_event(lv_event_t* e) {
  const lv_event_code_t code = lv_event_get_code(e);
  if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING) return;
  if (!s_clipboard_target || !lv_indev_get_act()) return;
  lv_point_t point;
  lv_indev_get_point(lv_indev_get_act(), &point);
  lv_obj_t* label = lv_textarea_get_label(s_clipboard_target);
  lv_area_t area;
  lv_obj_get_coords(label, &area);
  if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
    s_handle_grab = point;
    lv_label_get_letter_pos(label, lv_event_get_target(e) == s_selection_handles[0]
                           ? s_selection_start : s_selection_end, &s_handle_anchor);
    s_handle_anchor.x += area.x1;
    s_handle_anchor.y += area.y1;
    return;
  }
  if (lv_event_get_code(e) != LV_EVENT_PRESSING) return;
  point.x = s_handle_anchor.x + point.x - s_handle_grab.x - area.x1;
  point.y = s_handle_anchor.y + point.y - s_handle_grab.y - area.y1;
  uint32_t index = lv_label_get_letter_on(label, &point);
  const uint32_t count = _lv_txt_get_encoded_length(lv_textarea_get_text(s_clipboard_target));
  index = LV_MIN(index, count);
  if (lv_event_get_target(e) == s_selection_handles[0])
    s_selection_start = LV_MIN(index, s_selection_end ? s_selection_end - 1 : 0);
  else
    s_selection_end = LV_MAX(index, LV_MIN(s_selection_start + 1, count));
  lv_textarea_set_cursor_pos(s_clipboard_target, index);
  selection_update();
}

static void selection_delete(lv_obj_t* target, uint32_t start, uint32_t end) {
  lv_textarea_set_cursor_pos(target, end);
  for (uint32_t i = start; i < end; ++i) lv_textarea_del_char(target);
}

static void clipboard_action(lv_event_t* e) {
  const uint16_t action = lv_btnmatrix_get_selected_btn(lv_event_get_target(e));
  lv_obj_t* target = s_clipboard_target;
  if (!target || action == LV_BTNMATRIX_BTN_NONE) return;
  const uint32_t start = s_selection_start;
  const uint32_t end = s_selection_end;
  if (action == 3 && !lv_textarea_get_password_mode(target)) {
    s_selection_start = 0;
    s_selection_end = _lv_txt_get_encoded_length(lv_textarea_get_text(target));
    selection_update();
    return;
  }
  if ((action == 0 || action == 1) && !lv_textarea_get_password_mode(target)) {
    const char* text = lv_textarea_get_text(target);
    const uint32_t first = _lv_txt_encoded_get_byte_id(text, start);
    size_t len = _lv_txt_encoded_get_byte_id(text, end) - first;
    text += first;
    if (len >= sizeof(s_clipboard)) {
      // Never delete more text than the clipboard can retain.
      if (action == 1) return;
      len = sizeof(s_clipboard) - 1;
      while (len && (static_cast<unsigned char>(text[len]) & 0xC0) == 0x80) --len;
    }
    memcpy(s_clipboard, text, len);
    s_clipboard[len] = '\0';
  }
  clipboard_close();
  if (action == 1 || (action == 2 && s_clipboard[0])) selection_delete(target, start, end);
  if (action == 2 && s_clipboard[0]) lv_textarea_add_text(target, s_clipboard);
}

static void clipboard_field_event(lv_event_t* e) {
  lv_obj_t* target = lv_event_get_target(e);
  if (lv_event_get_code(e) == LV_EVENT_DELETE) {
    if (s_clipboard_target == target) clipboard_close();
    return;
  }
  const lv_event_code_t code = lv_event_get_code(e);
  if (s_clipboard_target == target && code == LV_EVENT_SCROLL) selection_update();
  if (s_clipboard_target == target &&
      (code == LV_EVENT_PRESSED || code == LV_EVENT_DEFOCUSED || code == LV_EVENT_VALUE_CHANGED))
    clipboard_close();
  if (code != LV_EVENT_LONG_PRESSED) return;
  clipboard_close();
  s_clipboard_target = target;
  const char* text = lv_textarea_get_text(target);
  const uint32_t count = _lv_txt_get_encoded_length(text);
  s_selection_start = s_selection_end = LV_MIN(lv_textarea_get_cursor_pos(target), count);
  if (!lv_textarea_get_password_mode(target) && count) {
    uint32_t index = s_selection_start == count ? count - 1 : s_selection_start;
    // ASCII separators delimit words; UTF-8 letters stay together.
    auto separator = [text](uint32_t c) {
      const unsigned char ch = text[_lv_txt_encoded_get_byte_id(text, c)];
      return ch <= ' ' || strchr(".,!?;:()[]{}\"", ch) != nullptr;
    };
    s_selection_start = index;
    s_selection_end = index + 1;
    if (!separator(index)) {
      while (s_selection_start && !separator(s_selection_start - 1)) --s_selection_start;
      while (s_selection_end < count && !separator(s_selection_end)) ++s_selection_end;
    }
    for (lv_obj_t*& handle : s_selection_handles) {
      handle = lv_obj_create(lv_layer_top());
      lv_obj_set_size(handle, 28, 28);
      lv_obj_set_style_radius(handle, LV_RADIUS_CIRCLE, 0);
      lv_obj_set_style_bg_color(handle, lv_color_hex(0x2196F3), 0);
      lv_obj_set_style_border_width(handle, 0, 0);
      lv_obj_set_style_bg_opa(handle, LV_OPA_COVER, 0);
      lv_obj_set_style_shadow_width(handle, 0, 0);
      lv_obj_set_style_outline_width(handle, 0, 0);
      lv_obj_clear_flag(handle, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);
      lv_obj_add_flag(handle, LV_OBJ_FLAG_PRESS_LOCK);
      lv_obj_set_ext_click_area(handle, 8);
      lv_obj_add_event_cb(handle, selection_handle_event, LV_EVENT_ALL, nullptr);
    }
    selection_update();
  }
  static const char* map[] = {"Copy", "Cut", "Paste", "All", "Close", ""};
  s_clipboard_menu = lv_btnmatrix_create(lv_layer_top());
  lv_btnmatrix_set_map(s_clipboard_menu, map);
  lv_obj_set_size(s_clipboard_menu, LV_MIN(SCR_W - 16, 420), 48);
  lv_area_t field_area;
  lv_obj_get_coords(target, &field_area);
  lv_obj_align(s_clipboard_menu, LV_ALIGN_TOP_MID, 0,
               LV_MAX(STATUS_H + 4, field_area.y1 - 56));
  lv_obj_clear_flag(s_clipboard_menu, LV_OBJ_FLAG_CLICK_FOCUSABLE);
  lv_btnmatrix_set_btn_ctrl_all(s_clipboard_menu, LV_BTNMATRIX_CTRL_NO_REPEAT | LV_BTNMATRIX_CTRL_CLICK_TRIG);
  if (lv_textarea_get_password_mode(target) || !count) {
    lv_btnmatrix_set_btn_ctrl(s_clipboard_menu, 0, LV_BTNMATRIX_CTRL_DISABLED);
    lv_btnmatrix_set_btn_ctrl(s_clipboard_menu, 1, LV_BTNMATRIX_CTRL_DISABLED);
    lv_btnmatrix_set_btn_ctrl(s_clipboard_menu, 3, LV_BTNMATRIX_CTRL_DISABLED);
  }
  if (!s_clipboard[0])
    lv_btnmatrix_set_btn_ctrl(s_clipboard_menu, 2, LV_BTNMATRIX_CTRL_DISABLED);
  lv_obj_add_event_cb(s_clipboard_menu, clipboard_action, LV_EVENT_VALUE_CHANGED, nullptr);
}

static void clipboard_register_fields(lv_obj_t* parent) {
  if (lv_obj_check_type(parent, &lv_textarea_class)) {
    lv_obj_set_style_bg_color(parent, lv_color_hex(0x2196F3), LV_PART_SELECTED);
    lv_obj_set_style_text_color(parent, lv_color_white(), LV_PART_SELECTED);
    lv_obj_remove_event_cb(parent, clipboard_field_event);
    lv_obj_add_event_cb(parent, clipboard_field_event, LV_EVENT_ALL, nullptr);
  }
  for (uint32_t i = 0; i < lv_obj_get_child_cnt(parent); ++i)
    clipboard_register_fields(lv_obj_get_child(parent, i));
}

static void clipboard_screen_unloaded(lv_event_t* e) {
  if (s_clipboard_target && lv_obj_get_screen(s_clipboard_target) == lv_event_get_target(e))
    clipboard_close();
}

void kb_apply_language(lv_obj_t* kb) {
  if (!kb) return;
  if (g_kb_greek && g_kb_lang != KB_LANG_EL) g_kb_lang = KB_LANG_EL;
  if (g_kb_lang == KB_LANG_EL) {
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_LOWER, (const char**)kb_gr_lc, kb_ctrl_lc);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_UPPER, (const char**)kb_gr_uc, kb_ctrl_uc);
  } else if (g_kb_lang == KB_LANG_DE) {
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_LOWER, (const char**)kb_de_lc, kb_ctrl_de_lc);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_UPPER, (const char**)kb_de_uc, kb_ctrl_de_uc);
  } else {
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_LOWER, (const char**)kb_en_lc, kb_ctrl_en_lc);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_UPPER, (const char**)kb_en_uc, kb_ctrl_en_uc);
  }
  lv_keyboard_set_popovers(kb, g_kb_lang == KB_LANG_EN);
  lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_SPECIAL, (const char**)custom_kb_map_sym, custom_kb_ctrl_sym);
  // Emoji page 1 as default
  lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_NUMBER, (const char**)kb_emoji_1, kb_ctrl_emoji);
}

static void kb_apply_language_all(lv_obj_t* active_kb) {
  lv_obj_t* keyboards[] = { ui_Keyboard1, ui_Keyboard2, ui_Keyboard3, ui_FeaturesKB };
  for (lv_obj_t* candidate : keyboards) {
    if (candidate) kb_apply_language(candidate);
  }
  if (active_kb) {
    lv_keyboard_set_mode(active_kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_invalidate(active_kb);
  }
}

static void cb_kb_value_changed(lv_event_t* e) {
    lv_obj_t* kb = lv_event_get_target(e);
    if (!kb) return;
    uint16_t btn_id = lv_btnmatrix_get_selected_btn(kb);
    if (btn_id == LV_BTNMATRIX_BTN_NONE) { lv_keyboard_def_event_cb(e); return; }
    const char* txt = lv_btnmatrix_get_btn_text(kb, btn_id);
    if (!txt) { lv_keyboard_def_event_cb(e); return; }

    if (strcmp(txt, "Aa") == 0) {
        lv_keyboard_t* keyboard = (lv_keyboard_t*)kb;
        if (keyboard->mode == LV_KEYBOARD_MODE_TEXT_LOWER)
            lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_UPPER);
        else
            lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
        return;
    }

    if (strcmp(txt, "Emoji") == 0) {
        s_emoji_page = 0;
        lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_NUMBER);
        return;
    }
    // Emoji page navigation: ▶ = next page, ◀ = prev page
    if (strcmp(txt, "\xE2\x96\xB6") == 0) {  // ▶
        s_emoji_page = 1;
        lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_NUMBER, (const char**)kb_emoji_2, kb_ctrl_emoji);
        lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_NUMBER);
        return;
    }
    if (strcmp(txt, "\xE2\x97\x80") == 0) {  // ◀
        s_emoji_page = 0;
        lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_NUMBER, (const char**)kb_emoji_1, kb_ctrl_emoji);
        lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_NUMBER);
        return;
    }
    if (strcmp(txt, "EL") == 0) {
        kb_set_language(KB_LANG_EL);
        kb_apply_language_all(kb);
        return;
    }
    if (strcmp(txt, "DE") == 0) {
        kb_set_language(KB_LANG_DE);
        kb_apply_language_all(kb);
        return;
    }
    if (strcmp(txt, "EN") == 0) {
        kb_set_language(KB_LANG_EN);
        kb_apply_language_all(kb);
        return;
    }

    lv_obj_t* target = lv_keyboard_get_textarea(kb);
    const bool edits_text = strcmp(txt, "1#") != 0 && strcmp(txt, "abc") != 0 &&
                            strcmp(txt, LV_SYMBOL_OK) != 0 && strcmp(txt, LV_SYMBOL_CLOSE) != 0;
    if (target && target == s_clipboard_target && edits_text) {
      const uint32_t start = s_selection_start;
      const uint32_t end = s_selection_end;
      clipboard_close();
      if (start < end) {
        selection_delete(target, start, end);
        if (strcmp(txt, LV_SYMBOL_BACKSPACE) == 0) return;
      }
    }
    lv_keyboard_def_event_cb(e);

    // Single-capitalize: revert to lowercase after typing a character
    {
      lv_keyboard_t* keyboard = (lv_keyboard_t*)kb;
      if (keyboard->mode == LV_KEYBOARD_MODE_TEXT_UPPER) {
        // Don't revert for control keys (backspace, enter, OK)
        if (strcmp(txt, LV_SYMBOL_BACKSPACE) != 0 &&
            strcmp(txt, LV_SYMBOL_NEW_LINE) != 0 &&
            strcmp(txt, LV_SYMBOL_OK) != 0) {
          lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
        }
      }
    }
}

void setup_keyboard(lv_obj_t* kb) {
  if (!kb) return;
  clipboard_register_fields(lv_obj_get_screen(kb));
  lv_obj_t* screen = lv_obj_get_screen(kb);
  lv_obj_remove_event_cb(screen, clipboard_screen_unloaded);
  lv_obj_add_event_cb(screen, clipboard_screen_unloaded, LV_EVENT_SCREEN_UNLOADED, nullptr);
  lv_obj_remove_event_cb(kb, lv_keyboard_def_event_cb);
  // Also strip our own handler first — setup_keyboard is called twice on
  // ui_Keyboard3 (once at boot in main.cpp, once from setup_repeater_screen_callbacks).
  // Without the remove, the handler fires twice per press: "Aa" toggles
  // upper→lower→upper and looks unresponsive; same for EL/EN which re-set
  // the same language back-to-back on every tap.
  lv_obj_remove_event_cb(kb, cb_kb_value_changed);
  lv_obj_add_event_cb(kb, cb_kb_value_changed, LV_EVENT_VALUE_CHANGED, nullptr);
  kb_apply_language(kb);
  // Force absolute placement of the keyboard widget — the LVGL default theme
  // applies its own border/outline on LV_PART_MAIN which eats ~30 px on the
  // left. Pin x=0, width=SCR_W, and null the border so the btnmatrix fills
  // the full panel width.
  lv_obj_set_align(kb, LV_ALIGN_TOP_LEFT);
  lv_obj_set_x(kb, 0);
  lv_obj_set_width(kb, SCR_W);
  lv_obj_set_height(kb, KB_HEIGHT);
  lv_obj_set_y(kb, KB_Y_OFFSET);
  // Kill every possible decoration on LV_PART_MAIN that could render as the
  // thin "button-like" slivers seen on the left edge of each row: default
  // theme border, outline, radius, and scrollbars. Also set the main bg to
  // match the screen so any pad_left gap becomes invisible against the
  // surrounding panel.
  lv_obj_set_style_border_width(kb, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_outline_width(kb, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_radius(kb, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_bg_color(kb, lv_color_hex(TH_BG), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_bg_opa(kb,   LV_OPA_COVER,        LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_scrollbar_mode(kb, LV_SCROLLBAR_MODE_OFF);
  lv_obj_clear_flag(kb, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_opa(kb, LV_OPA_TRANSP, LV_PART_SCROLLBAR | LV_STATE_DEFAULT);
  lv_obj_set_style_pad_top(kb,    0, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_pad_bottom(kb, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_pad_left(kb,   0, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_pad_right(kb,  0, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_pad_row(kb,    3, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_pad_column(kb,  6, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_radius(kb, 0, LV_PART_ITEMS | LV_STATE_DEFAULT);
  lv_obj_set_style_text_font(kb, &lv_font_montserrat_18, LV_PART_ITEMS | LV_STATE_DEFAULT);
  // Green highlight on key press
  lv_obj_set_style_bg_color(kb, lv_color_hex(0x2E7D32), LV_PART_ITEMS | LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_PRESSED);
}

void kb_hide(lv_obj_t* kb, lv_obj_t* ta) {
  clipboard_close();
  if (kb) {
    lv_keyboard_set_textarea(kb, nullptr);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
  }
  if (ta) {
    lv_obj_clear_state(ta, LV_STATE_FOCUSED);
  }
  lv_indev_t* indev = lv_indev_get_act();
  if (indev) lv_indev_wait_release(indev);
}

void kb_show(lv_obj_t* kb, lv_obj_t* ta) {
  if (!kb || !ta) return;
  clipboard_register_fields(lv_obj_get_screen(kb));
  kb_apply_language(kb);
  lv_keyboard_set_textarea(kb, ta);
  lv_obj_clear_flag(kb, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(kb);
}
