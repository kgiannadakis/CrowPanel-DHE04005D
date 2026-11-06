#include "graphics/mcui/McKeyboard.h"
#include "graphics/mcui/McTextEdit.h"
#include "graphics/mcui/McTheme.h"
#include "graphics/mcui/McUI.h"
#include <src/misc/lv_text_private.h>
#define LODEPNG_NO_COMPILE_CPP
#include <src/libs/lodepng/lodepng.h>
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int width = 479, height = 792;
static uint32_t ticks = 0;
static lv_point_t touch = {};
static lv_indev_state_t touch_state = LV_INDEV_STATE_RELEASED;
static lv_indev_t *pointer = nullptr;
static lv_display_t *display = nullptr;
static std::vector<uint32_t> draw_buffer, pixels;

namespace mcui {
int screen_width() { return width; }
int screen_height() { return height; }
int keyboard_height() { return width > height ? 210 : 330; }
const lv_font_t *font_16() { return &lv_font_montserrat_16; }
const lv_font_t *font_keyboard() { return &lv_font_montserrat_20; }
}

static void flush(lv_display_t *disp, const lv_area_t *area, uint8_t *data)
{
    auto *source = reinterpret_cast<uint32_t *>(data);
    for (int y = area->y1; y <= area->y2; ++y)
        for (int x = area->x1; x <= area->x2; ++x)
            pixels[y * width + x] = *source++;
    lv_display_flush_ready(disp);
}

static void pump(unsigned count = 1)
{
    for (unsigned i = 0; i < count; ++i) {
        ticks += 20;
        lv_indev_read(pointer);
        lv_timer_handler();
        mcui::text_edit_tick();
    }
}

static lv_obj_t *menu()
{
    auto *top = lv_layer_top();
    for (uint32_t i = 0; i < lv_obj_get_child_count(top); ++i) {
        auto *obj = lv_obj_get_child(top, i);
        if (lv_obj_check_type(obj, &lv_buttonmatrix_class)) return obj;
    }
    return nullptr;
}

static void action(uint32_t index)
{
    auto *obj = menu();
    assert(obj);
    lv_buttonmatrix_set_selected_button(obj, index);
    lv_obj_send_event(obj, LV_EVENT_VALUE_CHANGED, nullptr);
}

static void select_word(lv_obj_t *field, uint32_t cursor)
{
    mcui::keyboard_attach(field);
    lv_textarea_set_cursor_pos(field, cursor);
    lv_obj_send_event(field, LV_EVENT_LONG_PRESSED, nullptr);
    assert(menu());
}

static lv_obj_t *field(lv_obj_t *parent, const char *text)
{
    auto *obj = lv_textarea_create(parent);
    lv_textarea_set_one_line(obj, true);
    lv_obj_set_size(obj, width - 32, 48);
    lv_obj_set_pos(obj, 16, height - mcui::keyboard_height() - 70);
    lv_obj_set_style_text_font(obj, mcui::font_16(), 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(TH_INPUT), 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(TH_TEXT), 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 4, 0);
    lv_obj_set_style_pad_all(obj, 12, 0);
    lv_textarea_set_text(obj, text);
    return obj;
}

static void key(const char *value)
{
    auto *kb = mcui::keyboard_get();
    for (uint32_t i = 0; i < 60; ++i) {
        const char *button = lv_buttonmatrix_get_button_text(kb, i);
        if (!button) break;
        if (std::strcmp(button, value)) continue;
        lv_buttonmatrix_set_selected_button(kb, i);
        lv_obj_send_event(kb, LV_EVENT_PRESSED, nullptr);
        lv_obj_send_event(kb, LV_EVENT_RELEASED, nullptr);
        return;
    }
    assert(false && "keyboard key not found");
}

static void snapshot(const char *path)
{
    pump(2);
    lv_refr_now(display);
    assert(std::any_of(pixels.begin(), pixels.end(), [&](uint32_t pixel) { return pixel != pixels[0]; }));
    std::vector<unsigned char> rgba(pixels.size() * 4);
    for (size_t i = 0; i < pixels.size(); ++i) {
        rgba[i * 4] = (pixels[i] >> 16) & 255;
        rgba[i * 4 + 1] = (pixels[i] >> 8) & 255;
        rgba[i * 4 + 2] = pixels[i] & 255;
        rgba[i * 4 + 3] = 255;
    }
    unsigned char *png = nullptr;
    size_t png_size = 0;
    assert(lodepng_encode32(&png, &png_size, rgba.data(), width, height) == 0);
    FILE *output = std::fopen(path, "wb");
    assert(output && std::fwrite(png, 1, png_size, output) == png_size);
    assert(std::fclose(output) == 0);
    lv_free(png);
    lv_area_t area;
    lv_obj_get_coords(menu(), &area);
    assert(area.x1 >= 0 && area.x2 < width && area.y1 >= 0 && area.y2 < height);
}

int main()
{
    lv_init();
    lv_tick_set_cb([]() { return ticks; });
    display = lv_display_create(width, height);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_XRGB8888);
    draw_buffer.resize(width * height);
    pixels.resize(width * height);
    lv_display_set_buffers(display, draw_buffer.data(), nullptr, draw_buffer.size() * 4, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display, flush);
    pointer = lv_indev_create();
    lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(pointer, [](lv_indev_t *, lv_indev_data_t *data) {
        data->point = touch;
        data->state = touch_state;
    });
    lv_indev_set_long_press_time(pointer, 350);
    auto *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0E1621), 0);
    mcui::keyboard_create(screen);
    auto *a = field(screen, "one hello two");
    select_word(a, 6);
    auto *lbl = lv_textarea_get_label(a);
    assert(lv_label_get_text_selection_start(lbl) == 4 && lv_label_get_text_selection_end(lbl) == 9);
    action(0);
    assert(!menu() && std::strcmp(lv_textarea_get_text(a), "one hello two") == 0);
    auto *b = field(screen, "prefix ");
    select_word(b, 7);
    // Select a cursor-only paste location by using an empty field first.
    lv_textarea_set_text(b, "");
    select_word(b, 0);
    action(2);
    assert(std::strcmp(lv_textarea_get_text(b), "hello") == 0);

    lv_textarea_set_text(a, "one hello two");
    select_word(a, 6);
    action(1);
    assert(std::strcmp(lv_textarea_get_text(a), "one  two") == 0);
    lv_textarea_set_text(a, "one hello two");
    select_word(a, 6);
    key("q");
    assert(std::strcmp(lv_textarea_get_text(a), "one q two") == 0);
    lv_textarea_set_text(a, "one hello two");
    select_word(a, 6);
    key(LV_SYMBOL_BACKSPACE);
    assert(std::strcmp(lv_textarea_get_text(a), "one  two") == 0);

    const char *unicode = "\xCE\xB3\xCE\xB5\xCE\xB9\xCE\xB1 \xC3\xA4\xC3\xB6\xC3\xBC\xC3\x9F \xF0\x9F\x98\x80";
    lv_textarea_set_text(a, unicode);
    select_word(a, 2);
    action(3);
    action(0);
    lv_textarea_set_text(b, "");
    select_word(b, 0);
    action(2);
    assert(std::strcmp(lv_textarea_get_text(b), unicode) == 0);
    select_word(b, 12);
    action(1);
    assert(std::strcmp(lv_textarea_get_text(b), "\xCE\xB3\xCE\xB5\xCE\xB9\xCE\xB1 \xC3\xA4\xC3\xB6\xC3\xBC\xC3\x9F ") == 0);

    lv_textarea_set_text(a, "secret");
    lv_textarea_set_password_mode(a, true);
    select_word(a, 6);
    assert(lv_buttonmatrix_has_button_ctrl(menu(), 0, LV_BUTTONMATRIX_CTRL_DISABLED));
    action(0); // Even a programmatically forced Copy event cannot leak a password.
    action(2);
    assert(std::strcmp(lv_textarea_get_text(a), "secret\xF0\x9F\x98\x80") == 0);
    mcui::text_edit_close();
    lv_textarea_set_text(b, "");
    select_word(b, 0);
    action(2);
    assert(std::strcmp(lv_textarea_get_text(b), "\xF0\x9F\x98\x80") == 0);
    lv_textarea_set_password_mode(a, false);

    auto *message = lv_label_create(screen);
    lv_obj_set_pos(message, 16, 90);
    lv_obj_set_style_text_color(message, lv_color_hex(TH_TEXT), 0);
    lv_label_set_text(message, "Message to copy");
    mcui::text_edit_register_message(message);
    lv_obj_send_event(message, LV_EVENT_LONG_PRESSED, nullptr);
    assert(mcui::text_edit_message_selected());
    assert(lv_buttonmatrix_has_button_ctrl(menu(), 1, LV_BUTTONMATRIX_CTRL_DISABLED));
    assert(lv_buttonmatrix_has_button_ctrl(menu(), 2, LV_BUTTONMATRIX_CTRL_DISABLED));
    action(3);
    action(0);
    lv_textarea_set_text(b, "");
    select_word(b, 0);
    action(2);
    assert(std::strcmp(lv_textarea_get_text(b), "Message to copy") == 0);
    lv_textarea_set_max_length(b, 3);
    lv_textarea_set_text(b, "");
    select_word(b, 0);
    action(2);
    assert(std::strcmp(lv_textarea_get_text(b), "Mes") == 0);
    lv_textarea_set_max_length(b, 0);

    std::string oversized(1023, 'x');
    oversized += "\xF0\x9F\x98\x80";
    lv_textarea_set_text(a, oversized.c_str());
    select_word(a, 0);
    action(3);
    action(1);
    assert(std::strcmp(lv_textarea_get_text(a), oversized.c_str()) == 0);
    action(0);
    lv_textarea_set_text(b, "");
    select_word(b, 0);
    action(2);
    assert(std::strlen(lv_textarea_get_text(b)) == 1023);
    assert(lv_text_get_encoded_length(lv_textarea_get_text(b)) == 1023);

    // Actual pointer long-press and drag the right handle to the end of the line.
    lv_obj_delete(b);
    lv_textarea_set_text(a, "hello world");
    mcui::keyboard_attach(a);
    mcui::keyboard_show();
    lv_obj_update_layout(screen);
    lv_area_t bounds;
    lv_obj_get_coords(lbl, &bounds);
    touch = {bounds.x1 + 3, bounds.y1 + 5};
    touch_state = LV_INDEV_STATE_PRESSED;
    pump(24);
    assert(menu());
    touch_state = LV_INDEV_STATE_RELEASED;
    pump();
    assert(lv_obj_get_child_count(lv_layer_top()) == 3);
    auto *end_handle = lv_obj_get_child(lv_layer_top(), 1);
    lv_obj_get_coords(end_handle, &bounds);
    touch = {(bounds.x1 + bounds.x2) / 2, (bounds.y1 + bounds.y2) / 2};
    touch_state = LV_INDEV_STATE_PRESSED;
    pump();
    lv_point_t from, to;
    lv_label_get_letter_pos(lbl, 5, &from);
    lv_label_get_letter_pos(lbl, 11, &to);
    touch.x += to.x - from.x;
    pump();
    touch_state = LV_INDEV_STATE_RELEASED;
    pump();
    assert(lv_label_get_text_selection_end(lbl) == 11);
    snapshot(".pio/text-selection-portrait.png");
    mcui::text_edit_close();

    width = 792; height = 479;
    lv_display_set_resolution(display, width, height);
    lv_obj_set_size(a, width - 32, 48);
    lv_obj_set_pos(a, 16, height - mcui::keyboard_height() - 70);
    mcui::keyboard_show();
    select_word(a, 2);
    snapshot(".pio/text-selection-landscape.png");
    mcui::keyboard_hide();
    assert(!menu());
    select_word(a, 2);
    lv_obj_add_flag(a, LV_OBJ_FLAG_HIDDEN);
    mcui::text_edit_tick();
    assert(!menu());
    lv_obj_remove_flag(a, LV_OBJ_FLAG_HIDDEN);
    for (unsigned i = 0; i < 1000; ++i) {
        select_word(a, 2);
        mcui::text_edit_close();
        assert(lv_obj_get_child_count(lv_layer_top()) == 0);
    }
    select_word(a, 2);
    lv_obj_delete(a);
    assert(!menu() && !lv_obj_get_user_data(mcui::keyboard_get()));
    mcui::keyboard_hide();
    auto *modal = lv_obj_create(screen);
    mcui::keyboard_create(modal);
    auto *modal_field = field(modal, "modal draft");
    select_word(modal_field, 0);
    lv_obj_delete(modal); // Keyboard may be deleted before its attached field.
    assert(!menu() && !mcui::keyboard_get());
    std::puts("PASS: actual LVGL word/all selection, pointer handles, Copy/Cut/Paste, keyboard replacement, UTF-8, password protection, clipboard bounds, field lifetime, portrait/landscape rendering");
    lv_deinit();
}
