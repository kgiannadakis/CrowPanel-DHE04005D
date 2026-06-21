#if HAS_TFT && USE_MCUI

#include "McKeyboard.h"
#include "McTheme.h"
#include "McUI.h"

#include <cstring>

namespace mcui {

static lv_obj_t *s_kb = nullptr;
static bool s_visible = false;
enum KbLang { LANG_EN, LANG_EL, LANG_DE };
static KbLang s_lang = LANG_EN;

static const char *kb_map_en_lc[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", LV_SYMBOL_BACKSPACE, "\n",
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
    "a", "s", "d", "f", "g", "h", "j", "k", "l", LV_SYMBOL_NEW_LINE, "\n",
    "z", "x", "c", "v", "b", "n", "m", ".", ",", "!", "\n",
    "Aa", "EL", "1#", " ", "\xF0\x9F\x98\x80", "?", LV_SYMBOL_OK, ""
};

static const char *kb_map_en_uc[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", LV_SYMBOL_BACKSPACE, "\n",
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
    "A", "S", "D", "F", "G", "H", "J", "K", "L", LV_SYMBOL_NEW_LINE, "\n",
    "Z", "X", "C", "V", "B", "N", "M", ".", ",", "!", "\n",
    "Aa", "EL", "1#", " ", "\xF0\x9F\x98\x80", "?", LV_SYMBOL_OK, ""
};

static const char *kb_map_gr_lc[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", LV_SYMBOL_BACKSPACE, "\n",
    ";", "\xCF\x82", "\xCE\xB5", "\xCF\x81", "\xCF\x84", "\xCF\x85", "\xCE\xB8", "\xCE\xB9", "\xCE\xBF", "\xCF\x80", "\n",
    "\xCE\xB1", "\xCF\x83", "\xCE\xB4", "\xCF\x86", "\xCE\xB3", "\xCE\xB7", "\xCE\xBE", "\xCE\xBA", "\xCE\xBB", LV_SYMBOL_NEW_LINE, "\n",
    "\xCE\xB6", "\xCF\x87", "\xCF\x88", "\xCF\x89", "\xCE\xB2", "\xCE\xBD", "\xCE\xBC", ".", ",", "!", "\n",
    "Aa", "DE", "1#", " ", "\xF0\x9F\x98\x80", "?", LV_SYMBOL_OK, ""
};

static const char *kb_map_gr_uc[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", LV_SYMBOL_BACKSPACE, "\n",
    ":", "\xCE\xA3", "\xCE\x95", "\xCE\xA1", "\xCE\xA4", "\xCE\xA5", "\xCE\x98", "\xCE\x99", "\xCE\x9F", "\xCE\xA0", "\n",
    "\xCE\x91", "\xCE\xA3", "\xCE\x94", "\xCE\xA6", "\xCE\x93", "\xCE\x97", "\xCE\x9E", "\xCE\x9A", "\xCE\x9B", LV_SYMBOL_NEW_LINE, "\n",
    "\xCE\x96", "\xCE\xA7", "\xCE\xA8", "\xCE\xA9", "\xCE\x92", "\xCE\x9D", "\xCE\x9C", ".", ",", "!", "\n",
    "Aa", "DE", "1#", " ", "\xF0\x9F\x98\x80", "?", LV_SYMBOL_OK, ""
};

// German QWERTZ. Labels are UTF-8 byte escapes for ae/oe/ue and sharp-s.
// Sharp-s sits in the row-3 tail slot; OK sends instead of a newline key.
static const char *kb_map_de_lc[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", LV_SYMBOL_BACKSPACE, "\n",
    "q", "w", "e", "r", "t", "z", "u", "i", "o", "p", "\n",
    "a", "s", "d", "f", "g", "h", "j", "k", "l", "\xC3\x9F", "\n",
    "y", "x", "c", "v", "b", "n", "m", "\xC3\xA4", "\xC3\xB6", "\xC3\xBC", "\n",
    "Aa", "EN", "1#", " ", "\xF0\x9F\x98\x80", "?", LV_SYMBOL_OK, ""
};

static const char *kb_map_de_uc[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", LV_SYMBOL_BACKSPACE, "\n",
    "Q", "W", "E", "R", "T", "Z", "U", "I", "O", "P", "\n",
    "A", "S", "D", "F", "G", "H", "J", "K", "L", "\xC3\x9F", "\n",
    "Y", "X", "C", "V", "B", "N", "M", "\xC3\x84", "\xC3\x96", "\xC3\x9C", "\n",
    "Aa", "EN", "1#", " ", "\xF0\x9F\x98\x80", "?", LV_SYMBOL_OK, ""
};

static const char *kb_map_sym[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", LV_SYMBOL_BACKSPACE, "\n",
    "+", "-", "*", "/", "=", "%", "!", "?", "@", "#", "\n",
    "(", ")", "{", "}", "[", "]", "\\", ";", "\"", "'", LV_SYMBOL_NEW_LINE, "\n",
    "_", "~", "<", ">", "$", "^", "&", ".", ",", ":", "\n",
    "abc", " ", "?", LV_SYMBOL_OK, ""
};

// Emoji picker page (UTF-8 emoji as button labels - rendered in color via the
// emoji font fallback in MCUI_FONT_16). Codepoints match the SD atlas glyphs.
// "abc" returns to letters. Reached from the smiley key on letter layouts.
static const char *kb_map_emoji[] = {
    "\xF0\x9F\x98\x80","\xF0\x9F\x98\x83","\xF0\x9F\x98\x84","\xF0\x9F\x98\x81","\xF0\x9F\x98\x86","\xF0\x9F\x98\x85","\xF0\x9F\x98\x82","\xF0\x9F\xA4\xA3","\xF0\x9F\x98\x8A","\xF0\x9F\x98\x87", LV_SYMBOL_BACKSPACE, "\n",
    "\xF0\x9F\x98\x8D","\xF0\x9F\xA5\xB0","\xF0\x9F\x98\x98","\xF0\x9F\x98\x9A","\xF0\x9F\x98\x8B","\xF0\x9F\x98\x9B","\xF0\x9F\x98\x9C","\xF0\x9F\xA4\xAA","\xF0\x9F\x98\x9D","\xF0\x9F\xA4\x91", "\n",
    "\xF0\x9F\xA4\x97","\xF0\x9F\xA4\x94","\xF0\x9F\x98\x8E","\xF0\x9F\xA4\xA9","\xF0\x9F\x98\x8F","\xF0\x9F\x98\x92","\xF0\x9F\x98\x9E","\xF0\x9F\x98\xA2","\xF0\x9F\x98\xAD","\xF0\x9F\x98\xA4", "\n",
    "\xF0\x9F\x98\xA1","\xF0\x9F\x91\x8D","\xF0\x9F\x91\x8E","\xE2\x9D\xA4","\xF0\x9F\x94\xA5","\xF0\x9F\x8E\x89","\xF0\x9F\x92\xAF","\xE2\x9C\x85","\xF0\x9F\x92\x80","\xF0\x9F\x91\xBB", "\n",
    "abc", " ", LV_SYMBOL_RIGHT, LV_SYMBOL_OK, ""
};

// Emoji page 2: hands, hearts, objects, weather. LV_SYMBOL_LEFT returns to page 1.
static const char *kb_map_emoji2[] = {
    "\xF0\x9F\x91\x8D","\xF0\x9F\x91\x8E","\xF0\x9F\x91\x8F","\xF0\x9F\x99\x8C","\xF0\x9F\x91\x8A","\xE2\x9C\x8A","\xE2\x9C\x8C","\xF0\x9F\xA4\x9E","\xE2\x9C\x8B","\xF0\x9F\x99\x8F", LV_SYMBOL_BACKSPACE, "\n",
    "\xE2\x9D\xA4","\xF0\x9F\xA7\xA1","\xF0\x9F\x92\x9B","\xF0\x9F\x92\x9A","\xF0\x9F\x92\x99","\xF0\x9F\x92\x9C","\xF0\x9F\x96\xA4","\xF0\x9F\x92\x94","\xF0\x9F\x94\xA5","\xF0\x9F\x92\xAF", "\n",
    "\xE2\x9C\x85","\xE2\x9C\xA8","\xE2\xAD\x90","\xF0\x9F\x8C\x9F","\xF0\x9F\x8E\x89","\xF0\x9F\x8E\x81","\xF0\x9F\x8F\x86","\xF0\x9F\x92\xB0","\xF0\x9F\x93\xB1","\xF0\x9F\x92\xBB", "\n",
    "\xF0\x9F\x8D\xBB","\xF0\x9F\x8D\xBA","\xF0\x9F\x8D\xBD","\xF0\x9F\x8D\xBE","\xE2\x98\x80","\xE2\x9B\x85","\xE2\x9B\x84","\xE2\x9B\xBD","\xE2\x9B\xB5","\xE2\x9B\xAA", "\n",
    "abc", " ", LV_SYMBOL_LEFT, LV_SYMBOL_OK, ""
};

#define W1 LV_BUTTONMATRIX_CTRL_WIDTH_1
#define W2 LV_BUTTONMATRIX_CTRL_WIDTH_2
#define W3 LV_BUTTONMATRIX_CTRL_WIDTH_3
#define W4 LV_BUTTONMATRIX_CTRL_WIDTH_4
#define W5 LV_BUTTONMATRIX_CTRL_WIDTH_5
#define W6 LV_BUTTONMATRIX_CTRL_WIDTH_6
#define W7 LV_BUTTONMATRIX_CTRL_WIDTH_7
#define KC(v) static_cast<lv_buttonmatrix_ctrl_t>(v)
#define K1 KC(LV_BUTTONMATRIX_CTRL_NO_REPEAT | LV_BUTTONMATRIX_CTRL_POPOVER | W1)
#define K3 KC(LV_BUTTONMATRIX_CTRL_NO_REPEAT | LV_BUTTONMATRIX_CTRL_POPOVER | W3)
#define K4 KC(LV_BUTTONMATRIX_CTRL_NO_REPEAT | LV_BUTTONMATRIX_CTRL_POPOVER | W4)
#define K5 KC(LV_BUTTONMATRIX_CTRL_NO_REPEAT | LV_BUTTONMATRIX_CTRL_POPOVER | W5)
#define K6 KC(LV_BUTTONMATRIX_CTRL_NO_REPEAT | LV_BUTTONMATRIX_CTRL_POPOVER | W6)
#define K7 KC(LV_BUTTONMATRIX_CTRL_NO_REPEAT | LV_BUTTONMATRIX_CTRL_CLICK_TRIG | W7)
#define KACT2 KC(LV_BUTTONMATRIX_CTRL_NO_REPEAT | LV_BUTTONMATRIX_CTRL_CLICK_TRIG | W2)
#define KSHIFT KC(LV_BUTTONMATRIX_CTRL_NO_REPEAT | LV_BUTTONMATRIX_CTRL_CHECKABLE | W2)

static const lv_buttonmatrix_ctrl_t kb_ctrl_letters[] = {
    K1, K1, K1, K1, K1, K1, K1, K1, K1, K1, KACT2,
    K4, K4, K4, K4, K4, K4, K4, K4, K4, K4,
    K3, K3, K3, K3, K3, K3, K3, K3, K3, K7,
    K1, K1, K1, K1, K1, K1, K1, K1, K1, K1,
    KSHIFT, KACT2, KACT2, K4, K1, K1, KACT2
};

static const lv_buttonmatrix_ctrl_t kb_ctrl_sym[] = {
    K1, K1, K1, K1, K1, K1, K1, K1, K1, K1, KACT2,
    K1, K1, K1, K1, K1, K1, K1, K1, K1, K1,
    K1, K1, K1, K1, K1, K1, K1, K1, K1, K1, K7,
    K1, K1, K1, K1, K1, K1, K1, K1, K1, K1,
    KACT2, K6, K1, KACT2
};

// German letters: same shape as kb_ctrl_letters but row 3 is 10 normal keys
// (a-l + sharp-s) instead of 9 + a wide newline.
static const lv_buttonmatrix_ctrl_t kb_ctrl_de[] = {
    K1, K1, K1, K1, K1, K1, K1, K1, K1, K1, KACT2,
    K4, K4, K4, K4, K4, K4, K4, K4, K4, K4,
    K3, K3, K3, K3, K3, K3, K3, K3, K3, K3,
    K1, K1, K1, K1, K1, K1, K1, K1, K1, K1,
    KSHIFT, KACT2, KACT2, K4, K1, K1, KACT2
};

// Emoji page: 10 emoji per row + backspace, then abc/space/OK.
static const lv_buttonmatrix_ctrl_t kb_ctrl_emoji[] = {
    K1, K1, K1, K1, K1, K1, K1, K1, K1, K1, KACT2,
    K1, K1, K1, K1, K1, K1, K1, K1, K1, K1,
    K1, K1, K1, K1, K1, K1, K1, K1, K1, K1,
    K1, K1, K1, K1, K1, K1, K1, K1, K1, K1,
    KACT2, K5, K1, KACT2
};

#undef W1
#undef W2
#undef W3
#undef W4
#undef W5
#undef W6
#undef W7
#undef K1
#undef K3
#undef K4
#undef K5
#undef K6
#undef K7
#undef KACT2
#undef KSHIFT
#undef KC

enum KbMode { KB_LC, KB_UC, KB_SYM, KB_EMOJI };
static KbMode s_mode = KB_LC;
static int s_emoji_page = 0; // 0 = kb_map_emoji, 1 = kb_map_emoji2

static void apply_map(lv_obj_t *kb, KbMode m)
{
    s_mode = m;
    const lv_buttonmatrix_ctrl_t *letters_ctrl = (s_lang == LANG_DE) ? kb_ctrl_de : kb_ctrl_letters;
    switch (m) {
        case KB_LC:  lv_buttonmatrix_set_map(kb, s_lang == LANG_EL ? kb_map_gr_lc
                                                 : s_lang == LANG_DE ? kb_map_de_lc : kb_map_en_lc);
                     lv_buttonmatrix_set_ctrl_map(kb, letters_ctrl); break;
        case KB_UC:  lv_buttonmatrix_set_map(kb, s_lang == LANG_EL ? kb_map_gr_uc
                                                 : s_lang == LANG_DE ? kb_map_de_uc : kb_map_en_uc);
                     lv_buttonmatrix_set_ctrl_map(kb, letters_ctrl); break;
        case KB_SYM: lv_buttonmatrix_set_map(kb, kb_map_sym);
                     lv_buttonmatrix_set_ctrl_map(kb, kb_ctrl_sym); break;
        case KB_EMOJI: lv_buttonmatrix_set_map(kb, s_emoji_page ? kb_map_emoji2 : kb_map_emoji);
                       lv_buttonmatrix_set_ctrl_map(kb, kb_ctrl_emoji); break;
    }
}

static void kb_dispatch_key(lv_obj_t *kb, const char *txt)
{
    if (!txt) return;

    if (strcmp(txt, "Aa") == 0) {

        apply_map(kb, (s_mode == KB_UC) ? KB_LC : KB_UC);
        return;
    }
    if (strcmp(txt, "1#") == 0) {
        apply_map(kb, KB_SYM);
        return;
    }
    // The smiley key on letter rows opens the emoji page; the same glyph inside the
    // emoji page falls through below and is inserted as text.
    if (strcmp(txt, "\xF0\x9F\x98\x80") == 0 && s_mode != KB_EMOJI) {
        s_emoji_page = 0;
        apply_map(kb, KB_EMOJI);
        return;
    }
    // Emoji page navigation
    if (s_mode == KB_EMOJI && strcmp(txt, LV_SYMBOL_RIGHT) == 0) {
        s_emoji_page = 1;
        apply_map(kb, KB_EMOJI);
        return;
    }
    if (s_mode == KB_EMOJI && strcmp(txt, LV_SYMBOL_LEFT) == 0) {
        s_emoji_page = 0;
        apply_map(kb, KB_EMOJI);
        return;
    }
    if (strcmp(txt, "EL") == 0) {
        s_lang = LANG_EL;
        apply_map(kb, KB_LC);
        return;
    }
    if (strcmp(txt, "DE") == 0) {
        s_lang = LANG_DE;
        apply_map(kb, KB_LC);
        return;
    }
    if (strcmp(txt, "EN") == 0) {
        s_lang = LANG_EN;
        apply_map(kb, KB_LC);
        return;
    }
    if (strcmp(txt, "abc") == 0) {
        apply_map(kb, KB_LC);
        return;
    }
    if (strcmp(txt, LV_SYMBOL_OK) == 0) {

        lv_obj_t *ta = (lv_obj_t *)lv_obj_get_user_data(kb);
        if (ta) lv_obj_send_event(ta, LV_EVENT_READY, nullptr);
        return;
    }
    if (strcmp(txt, LV_SYMBOL_NEW_LINE) == 0) {

        lv_obj_t *ta = (lv_obj_t *)lv_obj_get_user_data(kb);
        if (ta) lv_textarea_add_char(ta, '\n');
        return;
    }
    if (strcmp(txt, LV_SYMBOL_BACKSPACE) == 0) {
        lv_obj_t *ta = (lv_obj_t *)lv_obj_get_user_data(kb);
        if (ta) lv_textarea_delete_char(ta);
        return;
    }

    lv_obj_t *ta = (lv_obj_t *)lv_obj_get_user_data(kb);
    if (ta) lv_textarea_add_text(ta, txt);
    if (s_mode == KB_UC) apply_map(kb, KB_LC);
}

static void kb_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *kb = (lv_obj_t *)lv_event_get_target(e);

    if (code == LV_EVENT_PRESSED || code == LV_EVENT_LONG_PRESSED_REPEAT) {
        uint32_t id = lv_buttonmatrix_get_selected_button(kb);
        if (id == LV_BUTTONMATRIX_BUTTON_NONE) return;
        const char *txt = lv_buttonmatrix_get_button_text(kb, id);
        kb_dispatch_key(kb, txt);
    }
}

static void keyboard_layout()
{
    if (!s_kb) return;
    lv_obj_set_align(s_kb, LV_ALIGN_TOP_LEFT);
    lv_obj_set_size(s_kb, SCR_W, keyboard_height());
    lv_obj_set_pos(s_kb, 0, SCR_H - keyboard_height());
}

lv_obj_t *keyboard_create(lv_obj_t *parent)
{

    s_kb = lv_buttonmatrix_create(parent);
    keyboard_layout();

    apply_map(s_kb, KB_LC);

    const lv_style_selector_t main_sel = static_cast<lv_style_selector_t>(LV_PART_MAIN);
    const lv_style_selector_t scrollbar_sel = static_cast<lv_style_selector_t>(LV_PART_SCROLLBAR);
    const lv_style_selector_t items_pressed_sel =
        static_cast<lv_style_selector_t>(LV_PART_ITEMS) | static_cast<lv_style_selector_t>(LV_STATE_PRESSED);

    lv_obj_set_style_bg_color(s_kb, lv_color_hex(TH_BG), main_sel);
    lv_obj_set_style_bg_opa(s_kb, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_kb, 0, main_sel);
    lv_obj_set_style_outline_width(s_kb, 0, main_sel);
    lv_obj_set_style_radius(s_kb, 0, main_sel);
    lv_obj_set_scrollbar_mode(s_kb, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(s_kb, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(s_kb, LV_OPA_TRANSP, scrollbar_sel);
    lv_obj_set_style_pad_top(s_kb, 0, main_sel);
    lv_obj_set_style_pad_bottom(s_kb, 0, main_sel);
    lv_obj_set_style_pad_left(s_kb, 0, main_sel);
    lv_obj_set_style_pad_right(s_kb, 0, main_sel);
    lv_obj_set_style_pad_row(s_kb, 3, main_sel);
    lv_obj_set_style_pad_column(s_kb, 6, main_sel);

    lv_obj_set_style_bg_color(s_kb, lv_color_hex(TH_INPUT), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(s_kb, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_text_color(s_kb, lv_color_hex(TH_TEXT), LV_PART_ITEMS);
    lv_obj_set_style_text_font(s_kb, MCUI_FONT_16, LV_PART_ITEMS);
    lv_obj_set_style_radius(s_kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_border_width(s_kb, 0, LV_PART_ITEMS);

    lv_obj_set_style_bg_color(s_kb, lv_color_hex(0x2E7D32), items_pressed_sel);
    lv_obj_set_style_bg_opa(s_kb, LV_OPA_COVER, items_pressed_sel);

    lv_obj_add_event_cb(s_kb, kb_event_cb, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(s_kb, kb_event_cb, LV_EVENT_LONG_PRESSED_REPEAT, nullptr);
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    s_visible = false;
    return s_kb;
}

void keyboard_attach(lv_obj_t *textarea)
{
    if (s_kb) lv_obj_set_user_data(s_kb, textarea);
}

void keyboard_show()
{
    if (!s_kb) return;
    keyboard_layout();
    lv_obj_remove_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_kb);
    s_visible = true;
}

void keyboard_hide()
{
    if (!s_kb) return;
    lv_obj_t *ta = (lv_obj_t *)lv_obj_get_user_data(s_kb);
    if (ta) lv_obj_remove_state(ta, LV_STATE_FOCUSED);
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    s_visible = false;
}

bool keyboard_is_visible() { return s_visible; }
lv_obj_t *keyboard_get() { return s_kb; }

}

#endif
