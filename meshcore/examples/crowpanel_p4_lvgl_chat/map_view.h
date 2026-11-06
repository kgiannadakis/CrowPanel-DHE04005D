#pragma once
// ============================================================
// map_view.h - Offline map screen and repeater neighbour overlays
// ------------------------------------------------------------
// SD-backed OSM tiles with pan/zoom, clustered contact markers, optional
// names and neighbour link overlays. Radio callbacks deposit bounded data;
// the LVGL task applies map updates.
//
// Tile path convention (matches OSM's slippy-map scheme):
//   /sdcard/tiles/{z}/{x}/{y}.png
//
// See tools/README-maps.md for tile preparation.
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

// UI thread starts a view; radio thread only deposits a bounded reply.
void map_show_neighbours(uint32_t tag, const uint8_t* source, const char* name, uint32_t timeout_ms);
void map_receive_neighbours(const uint8_t* source, const uint8_t* data, size_t len);
void map_receive_repeater_position(const uint8_t* source, const char* text);
void map_record_repeater_advert(const uint8_t* key, const char* name, int32_t lat, int32_t lon, uint32_t timestamp);
void map_clear_advert_locations(void);
bool map_get_repeater_advert(const uint8_t* key, char* name, size_t name_size,
                             int32_t* lat, int32_t* lon, uint32_t* timestamp);

#ifdef __cplusplus
}
#endif
