#if HAS_TFT && USE_MCUI

#include "McKeyboard.h"
#include "McTheme.h"
#include "McUI.h"

#include <cstring>

namespace mcui {

static lv_obj_t *s_kb = nullptr;
static bool s_visible = false;

static const char *kb_map_lc[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", LV_SYMBOL_BACKSPACE, "\n",
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
    "a", "s", "d", "f", "g", "h", "j", "k", "l", LV_SYMBOL_NEW_LINE, "\n",
    "z", "x", "c", "v", "b", "n", "m", ".", ",", "!", "\n",
    "Aa", "1#", " ", "?", LV_SYMBOL_OK, ""
};

static const char *kb_map_uc[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", LV_SYMBOL_BACKSPACE, "\n",
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
    "A", "S", "D", "F", "G", "H", "J", "K", "L", LV_SYMBOL_NEW_LINE, "\n",
    "Z", "X", "C", "V", "B", "N", "M", ".", ",", "!", "\n",
    "Aa", "1#", " ", "?", LV_SYMBOL_OK, ""
};

static const char *kb_map_sym[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", LV_SYMBOL_BACKSPACE, "\n",
    "+", "-", "*", "/", "=", "%", "!", "?", "@", "#", "\n",
    "(", ")", "{", "}", "[", "]", "\\", ";", "\"", "'", LV_SYMBOL_NEW_LINE, "\n",
    "_", "~", "<", ">", "$", "^", "&", ".", ",", ":", "\n",
    "abc", " ", "?", LV_SYMBOL_OK, ""
};

#define W1 LV_BUTTONMATRIX_CTRL_WIDTH_1
#define W2 LV_BUTTONMATRIX_CTRL_WIDTH_2
#define W6 LV_BUTTONMATRIX_CTRL_WIDTH_6

static const lv_buttonmatrix_ctrl_t kb_ctrl_letters[] = {

    W1,W1,W1,W1,W1,W1,W1,W1,W1,W1, W2,

    W1,W1,W1,W1,W1,W1,W1,W1,W1,W1,

    W1,W1,W1,W1,W1,W1,W1,W1,W1, W2,

    W1,W1,W1,W1,W1,W1,W1,W1,W1,W1,

    W2, W2, W6, W2, W2
};

static const lv_buttonmatrix_ctrl_t kb_ctrl_sym[] = {
    W1,W1,W1,W1,W1,W1,W1,W1,W1,W1, W2,
    W1,W1,W1,W1,W1,W1,W1,W1,W1,W1,
    W1,W1,W1,W1,W1,W1,W1,W1,W1,W1, W2,
    W1,W1,W1,W1,W1,W1,W1,W1,W1,W1,
    W2, W6, W2, W2
};

#undef W1
#undef W2
#undef W6

enum KbMode { KB_LC, KB_UC, KB_SYM };
static KbMode s_mode = KB_LC;

static void apply_map(lv_obj_t *kb, KbMode m)
{
    s_mode = m;
    switch (m) {
        case KB_LC:  lv_buttonmatrix_set_map(kb, kb_map_lc);
                     lv_buttonmatrix_set_ctrl_map(kb, kb_ctrl_letters); break;
        case KB_UC:  lv_buttonmatrix_set_map(kb, kb_map_uc);
                     lv_buttonmatrix_set_ctrl_map(kb, kb_ctrl_letters); break;
        case KB_SYM: lv_buttonmatrix_set_map(kb, kb_map_sym);
                     lv_buttonmatrix_set_ctrl_map(kb, kb_ctrl_sym); break;
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

lv_obj_t *keyboard_create(lv_obj_t *parent)
{

    s_kb = lv_buttonmatrix_create(parent);
    lv_obj_set_size(s_kb, SCR_W, keyboard_height());

    lv_obj_set_pos(s_kb, 0, SCR_H - keyboard_height());

    apply_map(s_kb, KB_LC);

    lv_obj_set_style_bg_color(s_kb, lv_color_hex(TH_SURFACE2), 0);
    lv_obj_set_style_bg_opa(s_kb, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_kb, 0, 0);
    lv_obj_set_style_pad_all(s_kb, 4, 0);
    lv_obj_set_style_pad_gap(s_kb, 4, 0);

    lv_obj_set_style_bg_color(s_kb, lv_color_hex(TH_INPUT), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(s_kb, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_text_color(s_kb, lv_color_hex(TH_TEXT), LV_PART_ITEMS);
    lv_obj_set_style_text_font(s_kb, &lv_font_montserrat_16, LV_PART_ITEMS);
    lv_obj_set_style_radius(s_kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_border_width(s_kb, 0, LV_PART_ITEMS);

    lv_obj_set_style_bg_color(s_kb, lv_color_hex(TH_ACCENT), LV_PART_ITEMS | LV_STATE_PRESSED);

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
    lv_obj_remove_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_kb);
    s_visible = true;
}

void keyboard_hide()
{
    if (!s_kb) return;
    lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    s_visible = false;
}

bool keyboard_is_visible() { return s_visible; }
lv_obj_t *keyboard_get() { return s_kb; }

}

#endif
