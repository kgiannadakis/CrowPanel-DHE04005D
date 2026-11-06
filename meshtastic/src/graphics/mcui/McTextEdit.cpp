#if HAS_TFT && USE_MCUI

#include "McTextEdit.h"
#include "McTheme.h"
#include "McUI.h"
#include <src/misc/lv_text_private.h>
#include <cstring>

namespace mcui {
namespace {
char clipboard[1025] = {};
char edit_buffer[1025] = {};
lv_obj_t *target = nullptr;
lv_obj_t *menu = nullptr;
lv_obj_t *handles[2] = {};
uint32_t selection_start = 0;
uint32_t selection_end = 0;
lv_point_t handle_grab = {};
lv_point_t handle_anchor = {};

bool editable() { return target && lv_obj_check_type(target, &lv_textarea_class); }
bool password() { return editable() && lv_textarea_get_password_mode(target); }
lv_obj_t *label() { return editable() ? lv_textarea_get_label(target) : target; }
const char *text() { return editable() ? lv_textarea_get_text(target) : lv_label_get_text(target); }

void close(bool clear_selection)
{
    if (target && clear_selection) {
        lv_obj_t *lbl = label();
        lv_label_set_text_selection_start(lbl, LV_LABEL_TEXT_SELECTION_OFF);
        lv_label_set_text_selection_end(lbl, LV_LABEL_TEXT_SELECTION_OFF);
    }
    target = nullptr;
    for (auto &handle : handles) {
        if (handle) lv_obj_delete(handle);
        handle = nullptr;
    }
    if (menu) lv_obj_delete(menu);
    menu = nullptr;
}

void selection_update()
{
    if (!target) return;
    lv_obj_t *lbl = label();
    const uint32_t count = lv_text_get_encoded_length(text());
    selection_start = LV_MIN(selection_start, count);
    selection_end = LV_CLAMP(selection_start, selection_end, count);
    if (lv_label_get_text_selection_start(lbl) != selection_start)
        lv_label_set_text_selection_start(lbl, selection_start);
    if (lv_label_get_text_selection_end(lbl) != selection_end)
        lv_label_set_text_selection_end(lbl, selection_end);
    lv_obj_update_layout(target);
    lv_area_t label_area, field_area;
    lv_obj_get_coords(lbl, &label_area);
    lv_obj_get_coords(target, &field_area);
    const lv_font_t *font = lv_obj_get_style_text_font(lbl, LV_PART_MAIN);
    for (unsigned i = 0; i < 2; ++i) {
        if (!handles[i]) continue;
        lv_point_t pos;
        lv_label_get_letter_pos(lbl, i ? selection_end : selection_start, &pos);
        const int32_t x = label_area.x1 + pos.x;
        const int32_t y = label_area.y1 + pos.y + font->line_height;
        if (selection_start == selection_end || x < field_area.x1 || x > field_area.x2 ||
            y < field_area.y1 || y > field_area.y2 + font->line_height) {
            lv_obj_add_flag(handles[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(handles[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(handles[i], LV_CLAMP(0, x - 14, SCR_W - 28), LV_CLAMP(0, y - 5, SCR_H - 28));
        }
    }
}

void handle_event(lv_event_t *e)
{
    const auto code = lv_event_get_code(e);
    if (!target || (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING)) return;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t point;
    lv_indev_get_point(indev, &point);
    lv_obj_t *lbl = label();
    lv_area_t area;
    lv_obj_get_coords(lbl, &area);
    const bool start_handle = lv_event_get_current_target(e) == handles[0];
    if (code == LV_EVENT_PRESSED) {
        handle_grab = point;
        lv_label_get_letter_pos(lbl, start_handle ? selection_start : selection_end, &handle_anchor);
        handle_anchor.x += area.x1;
        handle_anchor.y += area.y1;
        return;
    }
    point.x += handle_anchor.x - handle_grab.x - area.x1;
    point.y += handle_anchor.y - handle_grab.y - area.y1;
    const uint32_t count = lv_text_get_encoded_length(text());
    const uint32_t index = LV_MIN(lv_label_get_letter_on(lbl, &point, true), count);
    if (start_handle)
        selection_start = LV_MIN(index, selection_end ? selection_end - 1 : 0);
    else
        selection_end = LV_MAX(index, LV_MIN(selection_start + 1, count));
    if (editable()) lv_textarea_set_cursor_pos(target, index);
    selection_update();
}

void erase_selection(lv_obj_t *field, uint32_t start, uint32_t end)
{
    const char *value = lv_textarea_get_text(field);
    const uint32_t count = lv_text_get_encoded_length(value);
    start = LV_MIN(start, count);
    end = LV_CLAMP(start, end, count);
    const size_t first = lv_text_encoded_get_byte_id(value, start);
    const size_t last = lv_text_encoded_get_byte_id(value, end);
    const size_t suffix = std::strlen(value + last);
    if (first + suffix < sizeof(edit_buffer)) {
        // One change event avoids relaying out the field once for every deleted character.
        std::memcpy(edit_buffer, value, first);
        std::memcpy(edit_buffer + first, value + last, suffix + 1);
        lv_textarea_set_text(field, edit_buffer);
        lv_textarea_set_cursor_pos(field, start);
    } else {
        lv_textarea_set_cursor_pos(field, end);
        for (uint32_t i = start; i < end; ++i) lv_textarea_delete_char(field);
    }
}

void action_event(lv_event_t *e)
{
    if (!target) return;
    const uint32_t action = lv_buttonmatrix_get_selected_button(menu);
    if (action == LV_BUTTONMATRIX_BUTTON_NONE || action >= 5) return;
    const bool can_read = !password();
    const bool can_edit = editable();
    if (action == 3 && can_read) {
        selection_start = 0;
        selection_end = lv_text_get_encoded_length(text());
        selection_update();
        return;
    }
    if ((action == 0 || action == 1) && (!can_read || (action == 1 && !can_edit))) return;
    if (action == 2 && (!can_edit || !clipboard[0])) return;
    if (action == 0 || action == 1) {
        const char *value = text();
        const size_t first = lv_text_encoded_get_byte_id(value, selection_start);
        size_t length = lv_text_encoded_get_byte_id(value, selection_end) - first;
        value += first;
        if (length >= sizeof(clipboard)) {
            if (action == 1) return; // Never cut more than the clipboard can retain.
            length = sizeof(clipboard) - 1;
            while (length && (static_cast<unsigned char>(value[length]) & 0xC0) == 0x80) --length;
        }
        std::memcpy(clipboard, value, length);
        clipboard[length] = '\0';
    }
    lv_obj_t *field = target;
    const uint32_t start = selection_start, end = selection_end;
    close(true);
    if (action == 1 || action == 2) erase_selection(field, start, end);
    if (action == 2) lv_textarea_add_text(field, clipboard);
    (void)e;
}

void open(lv_obj_t *obj)
{
    close(true);
    target = obj;
    const char *value = text();
    const uint32_t count = lv_text_get_encoded_length(value);
    uint32_t cursor = editable() ? lv_textarea_get_cursor_pos(target) : 0;
    if (!editable() && lv_indev_active()) {
        lv_point_t point;
        lv_indev_get_point(lv_indev_active(), &point);
        lv_area_t area;
        lv_obj_get_coords(target, &area);
        point.x -= area.x1;
        point.y -= area.y1;
        cursor = lv_label_get_letter_on(target, &point, true);
    }
    selection_start = selection_end = LV_MIN(cursor, count);
    if (!password() && count) {
        const uint32_t index = selection_start == count ? count - 1 : selection_start;
        auto separator = [value](uint32_t character) {
            const unsigned char c = value[lv_text_encoded_get_byte_id(value, character)];
            return c <= ' ' || std::strchr(".,!?;:()[]{}\"", c) != nullptr;
        };
        selection_start = index;
        selection_end = index + 1;
        if (!separator(index)) {
            while (selection_start && !separator(selection_start - 1)) --selection_start;
            while (selection_end < count && !separator(selection_end)) ++selection_end;
        }
        for (auto &handle : handles) {
            handle = lv_obj_create(lv_layer_top());
            lv_obj_set_size(handle, 28, 28);
            lv_obj_set_style_radius(handle, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(handle, lv_color_hex(TH_ACCENT), 0);
            lv_obj_set_style_bg_opa(handle, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(handle, 0, 0);
            lv_obj_set_style_shadow_width(handle, 0, 0);
            lv_obj_set_style_outline_width(handle, 0, 0);
            lv_obj_remove_flag(handle, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_remove_flag(handle, LV_OBJ_FLAG_CLICK_FOCUSABLE);
            lv_obj_add_flag(handle, LV_OBJ_FLAG_PRESS_LOCK);
            lv_obj_set_ext_click_area(handle, 8);
            lv_obj_add_event_cb(handle, handle_event, LV_EVENT_ALL, nullptr);
        }
    }
    selection_update();
    static const char *map[] = {"Copy", "Cut", "Paste", "All", "Close", ""};
    menu = lv_buttonmatrix_create(lv_layer_top());
    lv_buttonmatrix_set_map(menu, map);
    lv_obj_set_size(menu, LV_MIN(SCR_W - 16, 420), 48);
    lv_area_t area;
    lv_obj_get_coords(target, &area);
    lv_obj_align(menu, LV_ALIGN_TOP_MID, 0, LV_CLAMP(STATUS_H + 4, area.y1 - 56, SCR_H - 48));
    lv_obj_set_style_bg_color(menu, lv_color_hex(TH_SURFACE), 0);
    lv_obj_set_style_border_width(menu, 1, 0);
    lv_obj_set_style_border_color(menu, lv_color_hex(TH_BORDER), 0);
    lv_obj_set_style_radius(menu, 4, 0);
    lv_obj_set_style_pad_all(menu, 6, 0);
    lv_obj_set_style_pad_row(menu, 0, 0);
    lv_obj_set_style_pad_column(menu, 6, 0);
    lv_obj_set_style_text_font(menu, MCUI_FONT_16, LV_PART_ITEMS);
    lv_obj_set_style_text_color(menu, lv_color_hex(TH_TEXT), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(menu, lv_color_hex(TH_INPUT), LV_PART_ITEMS);
    lv_obj_set_style_radius(menu, 4, LV_PART_ITEMS);
    lv_obj_set_style_border_width(menu, 0, LV_PART_ITEMS);
    lv_obj_set_style_pad_top(menu, 0, LV_PART_ITEMS);
    lv_obj_set_style_pad_bottom(menu, 0, LV_PART_ITEMS);
    lv_obj_remove_flag(menu, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_buttonmatrix_set_button_ctrl_all(menu, static_cast<lv_buttonmatrix_ctrl_t>(
        LV_BUTTONMATRIX_CTRL_NO_REPEAT | LV_BUTTONMATRIX_CTRL_CLICK_TRIG));
    if (password() || !count) {
        lv_buttonmatrix_set_button_ctrl(menu, 0, LV_BUTTONMATRIX_CTRL_DISABLED);
        lv_buttonmatrix_set_button_ctrl(menu, 1, LV_BUTTONMATRIX_CTRL_DISABLED);
        lv_buttonmatrix_set_button_ctrl(menu, 3, LV_BUTTONMATRIX_CTRL_DISABLED);
    }
    if (!editable()) lv_buttonmatrix_set_button_ctrl(menu, 1, LV_BUTTONMATRIX_CTRL_DISABLED);
    if (!editable() || !clipboard[0]) lv_buttonmatrix_set_button_ctrl(menu, 2, LV_BUTTONMATRIX_CTRL_DISABLED);
    lv_obj_add_event_cb(menu, action_event, LV_EVENT_VALUE_CHANGED, nullptr);
}

void target_event(lv_event_t *e)
{
    auto *obj = static_cast<lv_obj_t *>(lv_event_get_current_target(e));
    const auto code = lv_event_get_code(e);
    if (obj == target) {
        if (code == LV_EVENT_DELETE) { close(false); return; }
        if (code == LV_EVENT_PRESSED || code == LV_EVENT_DEFOCUSED || code == LV_EVENT_VALUE_CHANGED)
            close(true);
        else if (code == LV_EVENT_SCROLL)
            selection_update();
    }
    if (code == LV_EVENT_LONG_PRESSED) open(obj);
}

void register_target(lv_obj_t *obj)
{
    lv_obj_t *lbl = lv_obj_check_type(obj, &lv_textarea_class) ? lv_textarea_get_label(obj) : obj;
    lv_obj_set_style_bg_color(lbl, lv_color_hex(TH_ACCENT), LV_PART_SELECTED);
    lv_obj_set_style_text_color(lbl, lv_color_hex(TH_TEXT), LV_PART_SELECTED);
    lv_obj_remove_event_cb(obj, target_event);
    lv_obj_add_event_cb(obj, target_event, LV_EVENT_ALL, nullptr);
}
} // namespace

void text_edit_register_field(lv_obj_t *textarea)
{
    if (!textarea) return;
    lv_textarea_set_text_selection(textarea, false);
    register_target(textarea);
}

void text_edit_register_message(lv_obj_t *message)
{
    lv_obj_add_flag(message, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(message, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_remove_flag(message, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    register_target(message);
}

void text_edit_close() { close(true); }
void text_edit_tick()
{
    if (target && !lv_obj_is_visible(target)) close(true);
    else selection_update();
}
bool text_edit_message_selected() { return target && !editable(); }

bool text_edit_before_key(lv_obj_t *textarea)
{
    if (target != textarea) return false;
    const uint32_t start = selection_start, end = selection_end;
    close(true);
    if (start == end) return false;
    erase_selection(textarea, start, end);
    return true;
}
} // namespace mcui
#endif
