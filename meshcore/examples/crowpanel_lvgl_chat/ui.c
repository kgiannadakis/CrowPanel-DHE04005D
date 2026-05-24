// ui.c — Master init: creates all screens at startup

#include "ui.h"

lv_obj_t * ui____initial_actions0 = NULL;

void ui_init(void) {
    lv_disp_t * dispp = lv_disp_get_default();
    lv_theme_t * theme = lv_theme_default_init(
        dispp, lv_color_hex(TH_ACCENT), lv_color_hex(TH_ACCENT_LIGHT),
        true,  /* dark mode */
        &lv_font_montserrat_16);
    lv_disp_set_theme(dispp, theme);

    // Create all three screens up front (never destroyed during use).
    // main.cpp registers callbacks after this returns, so all widget
    // pointers must be valid.
    ui_homescreen_screen_init();
    ui_settingscreen_screen_init();
    ui_repeaterscreen_screen_init();

    // Load homescreen as the initial view
    lv_disp_load_scr(ui_homescreen);
}

void ui_destroy(void) {
    ui_homescreen_screen_destroy();
    ui_settingscreen_screen_destroy();
    ui_repeaterscreen_screen_destroy();
}
