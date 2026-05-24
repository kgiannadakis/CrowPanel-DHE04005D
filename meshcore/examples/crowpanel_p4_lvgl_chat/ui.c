// ui.c — Master init: creates all screens at startup

#include "ui.h"

lv_obj_t * ui____initial_actions0 = NULL;

void ui_init(void) {
    extern void ets_printf(const char *, ...);
    ets_printf(">>> ui_init: lv_disp_get_default\n");
    lv_disp_t * dispp = lv_disp_get_default();

    ets_printf(">>> ui_init: lv_theme_default_init (font_p=%p)\n",
               (void*)&lv_font_montserrat_16);
    lv_theme_t * theme = lv_theme_default_init(
        dispp, lv_color_hex(TH_ACCENT), lv_color_hex(TH_ACCENT_LIGHT),
        true,  /* dark mode */
        &lv_font_montserrat_16);
    ets_printf(">>> ui_init: lv_disp_set_theme\n");
    lv_disp_set_theme(dispp, theme);

    ets_printf(">>> ui_init: ui_homescreen_screen_init\n");
    ui_homescreen_screen_init();
    ets_printf(">>> ui_init: ui_settingscreen_screen_init\n");
    ui_settingscreen_screen_init();
    ets_printf(">>> ui_init: ui_repeaterscreen_screen_init\n");
    ui_repeaterscreen_screen_init();

    ets_printf(">>> ui_init: lv_disp_load_scr\n");
    lv_disp_load_scr(ui_homescreen);
    ets_printf(">>> ui_init: done\n");
}

void ui_destroy(void) {
    ui_homescreen_screen_destroy();
    ui_settingscreen_screen_destroy();
    ui_repeaterscreen_screen_destroy();
}
