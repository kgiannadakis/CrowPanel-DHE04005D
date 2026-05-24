#pragma once
// ============================================================
// map_view.h — Phase 1 map screen
// ------------------------------------------------------------
// Standalone screen that displays a single hardcoded OSM-format
// PNG tile loaded from the SD card. The plumbing proves out SD
// mount + LVGL POSIX-FS driver + PNG decode in one screen; Phase 2
// adds the multi-tile composite, pan, and zoom.
//
// Tile path convention (matches OSM's slippy-map scheme):
//   /sdcard/tiles/{z}/{x}/{y}.png
//
// Phase 1 displays the Brussels-region tile at zoom 10:
//   /sdcard/tiles/10/521/343.png
// See tools/fetch_one_tile.py for how to produce it.
// ============================================================

#ifdef __cplusplus
extern "C" {
#endif

#include <lvgl.h>

// LVGL screen for the map viewer. Created lazily by
// ui_mapscreen_screen_init(). Lifetime-managed like the other
// SquareLine-style screens in this project.
extern lv_obj_t* ui_mapscreen;

// Build the map screen. Safe to call multiple times — if the screen
// already exists it's reused. Called from the "Maps" button in
// features_ui.c.
void ui_mapscreen_screen_init(void);

// Tear down. Mirrors ui_*screen_destroy helpers elsewhere.
void ui_mapscreen_screen_destroy(void);

#ifdef __cplusplus
}
#endif
