// ============================================================
// display.cpp — LVGL display driver, touch, keyboard maps
// ============================================================

#include "display.h"
#include "utils.h"
#include "chat_ui.h"
#include "home_ui.h"
#include "settings_cb.h"
#include "repeater_ui.h"
#include "features_ui.h"
#include "app_globals.h"

#include <Wire.h>
#include <extra/widgets/keyboard/lv_keyboard.h>
#include "LovyanGFX_Driver.h"

#include "persistence.h"

#include "ui.h"
#include "ui_homescreen.h"
#include "ui_settingscreen.h"
#include "ui_repeaterscreen.h"

extern LGFX gfx;

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
  int16_t TEXTSEND_Y_DEFAULT = 690;
  int16_t SEARCH_Y_OFFSET    = -324;
  int16_t SETTINGS_KB_TOP    = 520;
  int16_t CHATPANEL_START_Y  = 100;
  int16_t BTN_HALF_W         = 215;
}
bool    g_landscape_mode   = false;

static void init_layout_constants() {
  if (g_landscape_mode) {
    SCR_W = 800;  SCR_H = 480;
    STATUS_H = 40; TAB_H = 50;
    CONTENT_Y = STATUS_H;
    CONTENT_H = SCR_H - STATUS_H - TAB_H;   // 390
    KB_HEIGHT = 190;  KB_Y_OFFSET = SCR_H - KB_HEIGHT;
    KB_TA_Y = KB_Y_OFFSET - 50;
    TEXTSEND_Y_DEFAULT = SCR_H - TAB_H - 50;
    SEARCH_Y_OFFSET    = -178;
    SETTINGS_KB_TOP    = KB_Y_OFFSET;
    CHATPANEL_START_Y  = 80;
    BTN_HALF_W         = 380;
  } else {
    SCR_W = 480;  SCR_H = 800;
    STATUS_H = 50; TAB_H = 60;
    CONTENT_Y = STATUS_H;
    CONTENT_H = SCR_H - STATUS_H - TAB_H;   // 690
    KB_HEIGHT = 280;  KB_Y_OFFSET = SCR_H - KB_HEIGHT;
    KB_TA_Y = KB_Y_OFFSET - 50;
    TEXTSEND_Y_DEFAULT = SCR_H - TAB_H - 50;
    SEARCH_Y_OFFSET    = -324;
    SETTINGS_KB_TOP    = KB_Y_OFFSET;
    CHATPANEL_START_Y  = 100;
    BTN_HALF_W         = 215;
  }
}

// ---- Display driver callbacks ----
static lv_disp_draw_buf_t draw_buf;
static lv_color_t *buf0, *buf1;
static uint16_t tx_touch, ty_touch;

static void flush_cb(lv_disp_drv_t* drv, const lv_area_t* a, lv_color_t* p) {
  if (!g_screen_awake) { lv_disp_flush_ready(drv); return; }
  gfx.waitDMA();
  gfx.pushImageDMA(a->x1, a->y1, a->x2-a->x1+1, a->y2-a->y1+1, (lgfx::rgb565_t*)p);
  lv_disp_flush_ready(drv);
}

static void touch_cb(lv_indev_drv_t*, lv_indev_data_t* d) {
  bool pressed = gfx.getTouch(&tx_touch, &ty_touch);

  if (!g_screen_awake && pressed) {
    note_touch_activity();
    g_swallow_touch   = true;
    g_touch_was_press = false;
    d->state = LV_INDEV_STATE_REL;
    return;
  }
  if (g_swallow_touch) {
    if (!pressed) g_swallow_touch = false;
    d->state = LV_INDEV_STATE_REL;
    return;
  }

  if (pressed && !g_touch_was_press) {
    g_swipe_start_x = tx_touch;
    g_swipe_start_y = ty_touch;
    g_swipe_tracking = true;
  }
  if (!pressed && g_touch_was_press && g_swipe_tracking) {
    g_swipe_tracking = false;
    // Don't detect gestures when keyboard is visible
    bool kb_visible = (ui_Keyboard1 && !lv_obj_has_flag(ui_Keyboard1, LV_OBJ_FLAG_HIDDEN));
    if (!kb_visible) {
      int16_t dx = (int16_t)tx_touch - (int16_t)g_swipe_start_x;
      int16_t dy = (int16_t)ty_touch - (int16_t)g_swipe_start_y;
      // Swipe left → go back (works on all screens)
      if (dx < -80 && (dy > -60 && dy < 60)) {
        g_deferred_swipe_back = true;
      }
      // Swipe up from bottom edge → go to home screen
      if (dy < -80 && (dx > -60 && dx < 60) && g_swipe_start_y > (SCR_H - 100)) {
        g_deferred_swipe_home = true;
      }
    }
  }

  if (pressed && !g_touch_was_press) {
    if (g_in_chat_mode && ui_Keyboard1 &&
        !lv_obj_has_flag(ui_Keyboard1, LV_OBJ_FLAG_HIDDEN)) {
      bool on_kb = false, on_ta = false;
      if (ui_Keyboard1) {
        lv_area_t a; lv_obj_get_coords(ui_Keyboard1, &a);
        on_kb = (tx_touch >= a.x1 && tx_touch <= a.x2 && ty_touch >= a.y1 && ty_touch <= a.y2);
      }
      if (!on_kb && ui_textsendtype) {
        lv_area_t a; lv_obj_get_coords(ui_textsendtype, &a);
        on_ta = (tx_touch >= a.x1 && tx_touch <= a.x2 && ty_touch >= a.y1 && ty_touch <= a.y2);
      }
      if (!on_kb && !on_ta) g_dismiss_keyboard = true;
    }
  }
  g_touch_was_press = pressed;

  d->state = pressed ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
  d->point.x = tx_touch; d->point.y = ty_touch;
  if (pressed && (millis() - g_last_touch_ms) > 1000)
    note_touch_activity();
}

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
  lv_obj_add_event_cb(kb, cb_kb_value_changed, LV_EVENT_VALUE_CHANGED, nullptr);
  kb_apply_language(kb);
  lv_obj_set_height(kb, KB_HEIGHT);
  lv_obj_set_y(kb, KB_Y_OFFSET);
  lv_obj_set_style_pad_top(kb,    0, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_pad_bottom(kb, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_pad_left(kb,   2, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_pad_right(kb,  2, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_pad_row(kb,    3, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_pad_column(kb,  6, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_radius(kb, 8, LV_PART_ITEMS | LV_STATE_DEFAULT);
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

// ---- Display initialization ----
void init_display_and_ui() {
#if defined(ESP32)
  Wire.end();
  delay(10);
#endif
  Wire.begin(15, 16);
#if defined(ESP32)
  Wire.setClock(400000);
  Wire.setTimeOut(50);
#endif
  delay(50);

  while (!(i2c_ok(0x30) && i2c_ok(0x5D))) {
    i2c_cmd(0x19);
    pinMode(1, OUTPUT); digitalWrite(1, LOW); delay(120);
    pinMode(1, INPUT);  delay(100);
  }

  g_landscape_mode = load_landscape_nvs();
  init_layout_constants();

  gfx.init();
  gfx.setRotation(g_landscape_mode ? 0 : 1);
  gfx.initDMA();
  gfx.fillScreen(TFT_BLACK);

  lv_init();
  lv_font_greek_init();

  int buf_rows = 96000 / SCR_W;   // 200 portrait, 120 landscape
  int buf_fallback = 19200 / SCR_W; // 40 portrait, 24 landscape
  buf0 = (lv_color_t*)heap_caps_malloc(SCR_W * buf_rows * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
  buf1 = (lv_color_t*)heap_caps_malloc(SCR_W * buf_rows * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
  if (!buf0 || !buf1) {
    buf0 = (lv_color_t*)heap_caps_malloc(SCR_W * buf_fallback * sizeof(lv_color_t), MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA);
    buf1 = (lv_color_t*)heap_caps_malloc(SCR_W * buf_fallback * sizeof(lv_color_t), MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA);
  }
  if (!buf0 || !buf1) while (1) delay(1000);

  lv_disp_draw_buf_init(&draw_buf, buf0, buf1, SCR_W * buf_rows);

  static lv_disp_drv_t dd; lv_disp_drv_init(&dd);
  dd.hor_res = SCR_W; dd.ver_res = SCR_H;
  dd.flush_cb = flush_cb; dd.draw_buf = &draw_buf;
  lv_disp_drv_register(&dd);

  static lv_indev_drv_t id; lv_indev_drv_init(&id);
  id.type = LV_INDEV_TYPE_POINTER; id.read_cb = touch_cb;
  id.long_press_time = 500;
  lv_indev_drv_register(&id);

  {
    lv_disp_t* disp = lv_disp_get_default();
    if (disp && disp->refr_timer) lv_timer_set_period(disp->refr_timer, 20);
  }

  delay(50);

  g_backlight_level = BL_MAX;
  g_screen_awake = false;
  screen_wake_soft(g_backlight_level);
  g_last_touch_ms = millis();

  ui_init();
  setup_keyboard(ui_Keyboard1);
  setup_keyboard(ui_Keyboard2);
  if (ui_homescreen) lv_scr_load(ui_homescreen);

  if (ui_chatpanel) {
    lv_obj_set_flex_flow(ui_chatpanel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ui_chatpanel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(ui_chatpanel, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(ui_chatpanel, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_scroll_snap_y(ui_chatpanel, LV_SCROLL_SNAP_NONE);
    lv_obj_clear_flag(ui_chatpanel, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_add_flag(ui_chatpanel, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_set_style_pad_row(ui_chatpanel, 6, 0);
    lv_obj_set_style_pad_all(ui_chatpanel, 6, 0);
    lv_obj_clean(ui_chatpanel);
  }

  if (ui_Keyboard1) lv_obj_add_flag(ui_Keyboard1, LV_OBJ_FLAG_HIDDEN);
  if (ui_Keyboard2) lv_obj_add_flag(ui_Keyboard2, LV_OBJ_FLAG_HIDDEN);

  if (ui_textsendtype) {
    lv_obj_set_y(ui_textsendtype, TEXTSEND_Y_DEFAULT);
    lv_obj_add_flag(ui_textsendtype, LV_OBJ_FLAG_HIDDEN);
  }

  if (ui_searchfield) lv_obj_set_pos(ui_searchfield, 0, SEARCH_Y_OFFSET);

  {
    lv_obj_t* green_btns[] = {
      ui_fadvertbutton, ui_zeroadvertbutton, ui_presetpickbutton,
      ui_notificationstoggle,
      ui_repeatersbutton, ui_autocontacttoggle, ui_autorepeatertoggle,
      ui_repeateradvertbutton, ui_neighboursbutton, ui_rebootbutton,
      ui_repeaterloginbutton, ui_mutebutton, ui_statusbutton
    };
    for (auto btn : green_btns) {
      if (btn) lv_obj_set_style_bg_color(btn, lv_color_hex(g_theme->btn_active),
                                         LV_PART_MAIN | LV_STATE_DEFAULT);
    }
  }
  if (ui_purgedatabutton) {
    lv_obj_set_style_bg_color(ui_purgedatabutton, lv_color_hex(g_theme->btn_danger),
                              LV_PART_MAIN | LV_STATE_DEFAULT);
  }
  if (ui_Label9) {
    lv_label_set_text(ui_Label9, LV_SYMBOL_UPLOAD "\nFlood-Advert");
    lv_obj_set_style_text_align(ui_Label9, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(ui_Label9);
  }
  if (ui_Label8) {
    lv_label_set_text(ui_Label8, LV_SYMBOL_REFRESH "\n0-Advert");
    lv_obj_set_style_text_align(ui_Label8, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(ui_Label8);
  }
  if (ui_Label5)  lv_label_set_text(ui_Label5,  LV_SYMBOL_SETTINGS " Select");
  if (ui_Label11) lv_label_set_text(ui_Label11, LV_SYMBOL_CALL     "\nAuto Contacts");
  if (ui_Label20) lv_label_set_text(ui_Label20, LV_SYMBOL_BELL " Notify");
  btn_lbl(ui_purgedatabutton,   LV_SYMBOL_TRASH "\nPurge Data");
  btn_lbl(ui_repeatersbutton,   LV_SYMBOL_WIFI  "\nRepeaters");
  btn_lbl(ui_autorepeatertoggle, LV_SYMBOL_WIFI " Auto add Repeaters");
  btn_lbl(ui_autocontacttoggle,  LV_SYMBOL_CALL " Auto add Contacts");
  btn_lbl(ui_statusbutton,       LV_SYMBOL_REFRESH "\nStatus");

  // Speaker button
  if (ui_notificationstoggle) {
    lv_obj_t* parent = lv_obj_get_parent(ui_notificationstoggle);
    if (parent) {
      g_speaker_btn = lv_btn_create(parent);
      lv_obj_set_size(g_speaker_btn, BTN_HALF_W, 50);
      lv_obj_set_style_radius(g_speaker_btn, 14, 0);
      lv_obj_set_style_border_opa(g_speaker_btn, LV_OPA_TRANSP, 0);
      lv_obj_set_style_shadow_opa(g_speaker_btn, LV_OPA_TRANSP, 0);
      lv_obj_set_style_pad_all(g_speaker_btn, 4, 0);
      lv_obj_t* sl = lv_label_create(g_speaker_btn);
      lv_obj_set_style_text_font(sl, &lv_font_montserrat_14, 0);
      lv_obj_set_style_text_align(sl, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_set_style_text_color(sl, lv_color_white(), 0);
      lv_obj_center(sl);
      lv_obj_add_event_cb(g_speaker_btn, cb_speaker_toggle, LV_EVENT_CLICKED, nullptr);
      ui_apply_speaker_btn_state();
    }
  }

  // Orientation toggle button
  if (ui_orientationtoggle) {
    lv_obj_set_style_bg_color(ui_orientationtoggle, lv_color_hex(g_theme->btn_active), 0);
    lv_obj_add_event_cb(ui_orientationtoggle, cb_orientation_toggle, LV_EVENT_CLICKED, nullptr);
    ui_apply_orientation_btn_state();
  }

  // Discover Repeaters + Floor Noise buttons
  if (ui_autorepeatertoggle) {
    // In landscape, buttons go into the column beside serial monitor
    // In portrait, create a separate row after the auto_row
    lv_obj_t* btn_parent = ui_settings_ser_btncol;
    lv_obj_t* disc_row = nullptr;

    if (!btn_parent) {
      // Portrait: create a side-by-side row in the form
      lv_obj_t* auto_row = lv_obj_get_parent(ui_autorepeatertoggle);
      lv_obj_t* form = auto_row ? lv_obj_get_parent(auto_row) : nullptr;
      if (form) {
        disc_row = lv_obj_create(form);
        lv_obj_set_size(disc_row, lv_pct(100), 54);
        lv_obj_set_style_bg_opa(disc_row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_opa(disc_row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_all(disc_row, 0, 0);
        lv_obj_set_style_pad_column(disc_row, 10, 0);
        lv_obj_set_flex_flow(disc_row, LV_FLEX_FLOW_ROW);
        lv_obj_clear_flag(disc_row, LV_OBJ_FLAG_SCROLLABLE);
        btn_parent = disc_row;
      }
    }

    if (btn_parent) {
      lv_coord_t bw = g_landscape_mode ? lv_pct(96) : BTN_HALF_W;
      int16_t    bh = g_landscape_mode ? 46 : 50;

      // Discover Repeaters button
      g_discover_repeaters_btn = lv_btn_create(btn_parent);
      lv_obj_set_size(g_discover_repeaters_btn, bw, bh);
      lv_obj_set_style_radius(g_discover_repeaters_btn, 14, 0);
      lv_obj_set_style_bg_color(g_discover_repeaters_btn, lv_color_hex(g_theme->btn_active), 0);
      lv_obj_set_style_border_opa(g_discover_repeaters_btn, LV_OPA_TRANSP, 0);
      lv_obj_set_style_shadow_opa(g_discover_repeaters_btn, LV_OPA_TRANSP, 0);
      lv_obj_set_style_pad_all(g_discover_repeaters_btn, 4, 0);
      lv_obj_t* dl = lv_label_create(g_discover_repeaters_btn);
      lv_label_set_text(dl, LV_SYMBOL_GPS " Repeater Discovery");
      lv_obj_set_style_text_font(dl, &lv_font_montserrat_14, 0);
      lv_obj_set_style_text_align(dl, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_set_style_text_color(dl, lv_color_white(), 0);
      lv_obj_center(dl);
      lv_obj_add_event_cb(g_discover_repeaters_btn, cb_discover_repeaters, LV_EVENT_CLICKED, nullptr);

      // Floor Noise button
      lv_obj_t* noise_btn = lv_btn_create(btn_parent);
      lv_obj_set_size(noise_btn, bw, bh);
      lv_obj_set_style_radius(noise_btn, 14, 0);
      lv_obj_set_style_bg_color(noise_btn, lv_color_hex(g_theme->btn_active), 0);
      lv_obj_set_style_border_opa(noise_btn, LV_OPA_TRANSP, 0);
      lv_obj_set_style_shadow_opa(noise_btn, LV_OPA_TRANSP, 0);
      lv_obj_set_style_pad_all(noise_btn, 4, 0);
      lv_obj_t* nl = lv_label_create(noise_btn);
      lv_label_set_text(nl, LV_SYMBOL_AUDIO " Floor Noise");
      lv_obj_set_style_text_font(nl, &lv_font_montserrat_14, 0);
      lv_obj_set_style_text_align(nl, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_set_style_text_color(nl, lv_color_white(), 0);
      lv_obj_center(nl);
      lv_obj_add_event_cb(noise_btn, cb_floor_noise, LV_EVENT_CLICKED, nullptr);

      // Packet Forward toggle button
      g_pkt_fwd_btn = lv_btn_create(btn_parent);
      lv_obj_set_size(g_pkt_fwd_btn, bw, bh);
      lv_obj_set_style_radius(g_pkt_fwd_btn, 14, 0);
      lv_obj_set_style_border_opa(g_pkt_fwd_btn, LV_OPA_TRANSP, 0);
      lv_obj_set_style_shadow_opa(g_pkt_fwd_btn, LV_OPA_TRANSP, 0);
      lv_obj_set_style_pad_all(g_pkt_fwd_btn, 4, 0);
      lv_obj_t* fl = lv_label_create(g_pkt_fwd_btn);
      lv_label_set_text(fl, LV_SYMBOL_SHUFFLE " Packet Forward");
      lv_obj_set_style_text_font(fl, &lv_font_montserrat_14, 0);
      lv_obj_set_style_text_align(fl, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_set_style_text_color(fl, lv_color_white(), 0);
      lv_obj_center(fl);
      lv_obj_add_event_cb(g_pkt_fwd_btn, cb_packet_forward_toggle, LV_EVENT_CLICKED, nullptr);
      ui_apply_packet_forward_state();

      // Portrait: move disc_row after auto_row
      if (disc_row) {
        lv_obj_t* auto_row = lv_obj_get_parent(ui_autorepeatertoggle);
        lv_obj_move_to_index(disc_row, lv_obj_get_index(auto_row) + 1);
      }
    }
  }
}
