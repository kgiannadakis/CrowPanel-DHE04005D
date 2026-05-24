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
static lv_disp_draw_buf_t draw_buf;
static lv_color_t *buf0, *buf1;
// P4 port: v11's LVGL flush_cb/touch_cb lived here and were driven by
// LovyanGFX. On P4 the flush is handled by our display.cpp (which sits on
// esp_lcd_panel_rgb) and the touch indev is fed by the GT911 driver in
// display.cpp::touch_read_cb. The gesture logic (swipe-back / swipe-home,
// keyboard-dismiss-on-tap-outside, wake-on-touch) from v11's touch_cb has
// NOT been ported yet; to restore that behaviour we'll need to wrap the
// indev callback in display.cpp. TODO: reinstate gesture wiring.

// ---- Custom keyboard maps ----
#define _KB_N    (LV_BTNMATRIX_CTRL_NO_REPEAT | LV_BTNMATRIX_CTRL_POPOVER | 1)
#define _KB_LC   (LV_BTNMATRIX_CTRL_NO_REPEAT | LV_BTNMATRIX_CTRL_POPOVER | 4)
#define _KB_SM   (LV_BTNMATRIX_CTRL_NO_REPEAT | LV_BTNMATRIX_CTRL_POPOVER | 3)
#define _KB_XS   (LV_BTNMATRIX_CTRL_NO_REPEAT | LV_BTNMATRIX_CTRL_POPOVER | 1)
#define _KB_CTRL (LV_BTNMATRIX_CTRL_NO_REPEAT | LV_BTNMATRIX_CTRL_CLICK_TRIG | LV_BTNMATRIX_CTRL_CHECKED)
#define _KB_SHFT (LV_BTNMATRIX_CTRL_NO_REPEAT | LV_BTNMATRIX_CTRL_CHECKABLE | 2)

static const char * const kb_en_lc[] = {
    "1","2","3","4","5","6","7","8","9","0", LV_SYMBOL_BACKSPACE, "\n",
    "q","w","e","r","t","y","u","i","o","p", "\n",
    "a","s","d","f","g","h","j","k","l", LV_SYMBOL_NEW_LINE, "\n",
    "z","x","c","v","b","n","m",".",",","!", "\n",
    "Aa","EL","1#"," ","?", LV_SYMBOL_OK, ""
};
static const char * const kb_en_uc[] = {
    "1","2","3","4","5","6","7","8","9","0", LV_SYMBOL_BACKSPACE, "\n",
    "Q","W","E","R","T","Y","U","I","O","P", "\n",
    "A","S","D","F","G","H","J","K","L", LV_SYMBOL_NEW_LINE, "\n",
    "Z","X","C","V","B","N","M",".",",","!", "\n",
    "Aa","EL","1#"," ","?", LV_SYMBOL_OK, ""
};
static const char * const kb_gr_lc[] = {
    "1","2","3","4","5","6","7","8","9","0", LV_SYMBOL_BACKSPACE, "\n",
    ";","\xCF\x82","\xCE\xB5","\xCF\x81","\xCF\x84","\xCF\x85","\xCE\xB8","\xCE\xB9","\xCE\xBF","\xCF\x80", "\n",
    "\xCE\xB1","\xCF\x83","\xCE\xB4","\xCF\x86","\xCE\xB3","\xCE\xB7","\xCE\xBE","\xCE\xBA","\xCE\xBB", LV_SYMBOL_NEW_LINE, "\n",
    "\xCE\xB6","\xCF\x87","\xCF\x88","\xCF\x89","\xCE\xB2","\xCE\xBD","\xCE\xBC",".",",","!", "\n",
    "Aa","EN","1#"," ","?", LV_SYMBOL_OK, ""
};
static const char * const kb_gr_uc[] = {
    "1","2","3","4","5","6","7","8","9","0", LV_SYMBOL_BACKSPACE, "\n",
    ":","\xCE\xA3","\xCE\x95","\xCE\xA1","\xCE\xA4","\xCE\xA5","\xCE\x98","\xCE\x99","\xCE\x9F","\xCE\xA0", "\n",
    "\xCE\x91","\xCE\xA3","\xCE\x94","\xCE\xA6","\xCE\x93","\xCE\x97","\xCE\x9E","\xCE\x9A","\xCE\x9B", LV_SYMBOL_NEW_LINE, "\n",
    "\xCE\x96","\xCE\xA7","\xCE\xA8","\xCE\xA9","\xCE\x92","\xCE\x9D","\xCE\x9C",".",",","!", "\n",
    "Aa","EN","1#"," ","?", LV_SYMBOL_OK, ""
};

static const lv_btnmatrix_ctrl_t kb_ctrl_lc[] = {
    _KB_N,_KB_N,_KB_N,_KB_N,_KB_N,_KB_N,_KB_N,_KB_N,_KB_N,_KB_N, LV_BTNMATRIX_CTRL_CHECKED|2,
    _KB_LC,_KB_LC,_KB_LC,_KB_LC,_KB_LC,_KB_LC,_KB_LC,_KB_LC,_KB_LC,_KB_LC,
    _KB_SM,_KB_SM,_KB_SM,_KB_SM,_KB_SM,_KB_SM,_KB_SM,_KB_SM,_KB_SM, LV_BTNMATRIX_CTRL_CHECKED|7,
    _KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,
    _KB_SHFT, _KB_CTRL, _KB_CTRL, LV_BTNMATRIX_CTRL_NO_REPEAT|5, _KB_N, _KB_CTRL|2
};
static const lv_btnmatrix_ctrl_t kb_ctrl_uc[] = {
    _KB_N,_KB_N,_KB_N,_KB_N,_KB_N,_KB_N,_KB_N,_KB_N,_KB_N,_KB_N, LV_BTNMATRIX_CTRL_CHECKED|2,
    _KB_LC,_KB_LC,_KB_LC,_KB_LC,_KB_LC,_KB_LC,_KB_LC,_KB_LC,_KB_LC,_KB_LC,
    _KB_SM,_KB_SM,_KB_SM,_KB_SM,_KB_SM,_KB_SM,_KB_SM,_KB_SM,_KB_SM, LV_BTNMATRIX_CTRL_CHECKED|7,
    _KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,_KB_XS,
    _KB_SHFT|LV_BTNMATRIX_CTRL_CHECKED, _KB_CTRL, _KB_CTRL, LV_BTNMATRIX_CTRL_NO_REPEAT|5, _KB_N, _KB_CTRL|2
};

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
void kb_apply_language(lv_obj_t* kb) {
  if (!kb) return;
  if (g_kb_greek) {
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_LOWER, (const char**)kb_gr_lc, kb_ctrl_lc);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_UPPER, (const char**)kb_gr_uc, kb_ctrl_uc);
  } else {
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_LOWER, (const char**)kb_en_lc, kb_ctrl_lc);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_UPPER, (const char**)kb_en_uc, kb_ctrl_uc);
  }
  lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_SPECIAL, (const char**)custom_kb_map_sym, custom_kb_ctrl_sym);
  // Emoji page 1 as default
  lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_NUMBER, (const char**)kb_emoji_1, kb_ctrl_emoji);
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
        g_kb_greek = true;
        kb_apply_language(kb);
        if (kb != ui_Keyboard1 && ui_Keyboard1) kb_apply_language(ui_Keyboard1);
        if (kb != ui_Keyboard2 && ui_Keyboard2) kb_apply_language(ui_Keyboard2);
        if (kb != ui_Keyboard3 && ui_Keyboard3) kb_apply_language(ui_Keyboard3);
        if (kb != ui_FeaturesKB && ui_FeaturesKB) kb_apply_language(ui_FeaturesKB);
        lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
        return;
    }
    if (strcmp(txt, "EN") == 0) {
        g_kb_greek = false;
        kb_apply_language(kb);
        if (kb != ui_Keyboard1 && ui_Keyboard1) kb_apply_language(ui_Keyboard1);
        if (kb != ui_Keyboard2 && ui_Keyboard2) kb_apply_language(ui_Keyboard2);
        if (kb != ui_Keyboard3 && ui_Keyboard3) kb_apply_language(ui_Keyboard3);
        if (kb != ui_FeaturesKB && ui_FeaturesKB) kb_apply_language(ui_FeaturesKB);
        lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
        return;
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
  lv_keyboard_set_textarea(kb, ta);
  lv_obj_clear_flag(kb, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(kb);
}
