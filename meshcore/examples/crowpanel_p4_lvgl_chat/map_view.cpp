// ============================================================
// map_view.cpp — Simplified high-performance map screen
// ------------------------------------------------------------
// Goals:
//  - Fast/smooth panning over visual quality
//  - Show companions + repeaters as markers
//  - Show repeater names only at zoom 11 and 12
// ============================================================

#include "map_view.h"
#include "app_globals.h"
#include "ui_theme.h"
#include "ui_tabbar.h"
#include "ui_homescreen.h"
#include "sd_storage.h"
#include "utils.h"

#include <Preferences.h>
#include <helpers/AdvertDataHelpers.h>
#include <helpers/BaseChatMesh.h>

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

lv_obj_t* ui_mapscreen = nullptr;

namespace {

constexpr int kTilePx = 256;
constexpr int kMinZoom = 7;
// Allow z7..z12 as requested.
constexpr int kMaxZoom = 12;
constexpr double kInitLat = 51.21075;
constexpr double kInitLon = 4.43384;
constexpr int kInitZoom = 11;
// RAM "hot ring": keep decoded/open images alive to reduce
// SD re-open/re-decode churn during pan, but avoid over-allocating
// image cache under very fast motion bursts.
constexpr uint16_t kImgCacheEntries = 32;
constexpr uint32_t kDragApplyMs = 0;           // apply every drag event for max responsiveness
constexpr uint32_t kMarkerRestoreDelayMs = 120;
constexpr int kRebuildEdgeMarginPx = 24;
// More tile slots so we can keep a prefetch ring around the viewport.
constexpr int kMaxTileSlots = 40;
constexpr int kDesiredTilePad = 1;
constexpr int kMaxMarkerSlots = 40;
constexpr int kMarkerMinZoom = 9; // speed-first: no marker pipeline on low zooms

enum MarkerFilter {
  FILTER_ALL = 0,
  FILTER_COMPANIONS = 1,
  FILTER_REPEATERS = 2,
};

struct MapState {
  double center_lat = kInitLat;
  double center_lon = kInitLon;
  int zoom = kInitZoom;
  MarkerFilter filter = FILTER_ALL;

  lv_obj_t* content = nullptr;
  lv_obj_t* pan_layer = nullptr;
  lv_obj_t* zoom_label = nullptr;
  lv_obj_t* mode_label = nullptr;

  int drag_dx = 0;
  int drag_dy = 0;
};

struct TileSlot {
  lv_obj_t* img = nullptr;
  lv_obj_t* ph = nullptr;
  char src[128] = {0};
  bool has_real_tile = false;
  bool fast_hidden_img = false;
};

struct MarkerSlot {
  lv_obj_t* dot = nullptr;
  lv_obj_t* lbl = nullptr;
  char name[32] = {0};
  bool active_dot = false;
  bool active_lbl = false;
};

MapState s;
bool s_markers_hidden = false;
int s_pending_dx = 0;
int s_pending_dy = 0;
uint32_t s_last_drag_apply_ms = 0;
lv_timer_t* s_marker_restore_timer = nullptr;
TileSlot s_tile_slots[kMaxTileSlots];
bool s_tile_slots_ready = false;
MarkerSlot s_marker_slots[kMaxMarkerSlots];
bool s_marker_slots_ready = false;

static inline double world_size_px(int zoom) {
  return 256.0 * (double)(1 << zoom);
}

static void latlon_to_world(double lat, double lon, int zoom, double* wx, double* wy) {
  const double n = world_size_px(zoom);
  *wx = (lon + 180.0) / 360.0 * n;
  const double lat_rad = lat * M_PI / 180.0;
  *wy = (1.0 - asinh(tan(lat_rad)) / M_PI) / 2.0 * n;
}

static void world_to_latlon(double wx, double wy, int zoom, double* lat, double* lon) {
  const double n = world_size_px(zoom);
  *lon = wx / n * 360.0 - 180.0;
  const double y_norm = wy / n;
  const double lat_rad = atan(sinh(M_PI * (1.0 - 2.0 * y_norm)));
  *lat = lat_rad * 180.0 / M_PI;
}

static bool tile_exists_on_sd(int z, int x, int y) {
  char path[112];
  struct stat st;
  snprintf(path, sizeof(path), "/sdcard/tiles/%d/%d/%d.png", z, x, y);
  return stat(path, &st) == 0 && st.st_size > 0;
}

static bool pick_tile_source(int z, int x, int y, char* out, size_t out_sz) {
  if (!out || out_sz == 0) return false;
  struct stat st;
  // Original map mode: PNG tiles only.
  snprintf(out, out_sz, "S:/sdcard/tiles/%d/%d/%d.png", z, x, y);
  if (stat(out + 2, &st) == 0 && st.st_size > 0) return true;
  out[0] = 0;
  return false;
}

static void update_overlay_labels(int loaded, int total, int pins, int hidden) {
  if (s.zoom_label) {
    char b[96];
    if (hidden > 0) {
      snprintf(b, sizeof(b), "Z%d  %d/%d tiles  %d pins (+%d)", s.zoom, loaded, total, pins, hidden);
    } else {
      snprintf(b, sizeof(b), "Z%d  %d/%d tiles  %d pins", s.zoom, loaded, total, pins);
    }
    lv_label_set_text(s.zoom_label, b);
  }
  if (s.mode_label) {
    const char* m = "All";
    if (s.filter == FILTER_COMPANIONS) m = "Companions";
    else if (s.filter == FILTER_REPEATERS) m = "Repeaters";
    lv_label_set_text(s.mode_label, m);
  }
}

static void map_viewport_save_now() {
  Preferences p;
  p.begin("map", false);
  p.putInt("lat_e6", (int32_t)(s.center_lat * 1.0e6));
  p.putInt("lon_e6", (int32_t)(s.center_lon * 1.0e6));
  p.putUChar("zoom", (uint8_t)s.zoom);
  p.putUChar("filt", (uint8_t)s.filter);
  p.end();
}

static bool map_viewport_load() {
  Preferences p;
  if (!p.begin("map", true)) return false;
  const int32_t lat_e6 = p.getInt("lat_e6", INT32_MIN);
  const int32_t lon_e6 = p.getInt("lon_e6", INT32_MIN);
  uint8_t z = p.getUChar("zoom", 0xFF);
  uint8_t f = p.getUChar("filt", 0xFF);
  p.end();

  if (lat_e6 == INT32_MIN || lon_e6 == INT32_MIN || z == 0xFF) return false;
  if (z < kMinZoom) z = kMinZoom;
  if (z > kMaxZoom) z = kMaxZoom;
  if (f > FILTER_REPEATERS) f = FILTER_ALL;

  s.center_lat = (double)lat_e6 / 1.0e6;
  s.center_lon = (double)lon_e6 / 1.0e6;
  s.zoom = (int)z;
  s.filter = (MarkerFilter)f;
  return true;
}

static void map_viewport_mark_dirty() {
  g_map_viewport_save_dirty = true;
  g_map_viewport_save_ms = millis();
}

static void set_marker_visibility(bool visible) {
  for (int i = 0; i < kMaxMarkerSlots; i++) {
    MarkerSlot& m = s_marker_slots[i];
    if (!m.dot || !m.lbl) continue;
    if (!m.active_dot) continue;
    if (visible) lv_obj_clear_flag(m.dot, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(m.dot, LV_OBJ_FLAG_HIDDEN);
    if (m.active_lbl) {
      if (visible) lv_obj_clear_flag(m.lbl, LV_OBJ_FLAG_HIDDEN);
      else lv_obj_add_flag(m.lbl, LV_OBJ_FLAG_HIDDEN);
    }
  }
}

static void ensure_tile_slots() {
  if (!s.pan_layer || s_tile_slots_ready) return;
  for (int i = 0; i < kMaxTileSlots; i++) {
    TileSlot& t = s_tile_slots[i];
    t.img = lv_img_create(s.pan_layer);
    lv_obj_clear_flag(t.img, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(t.img, LV_OBJ_FLAG_HIDDEN);
    lv_img_set_antialias(t.img, false);

    t.ph = lv_obj_create(s.pan_layer);
    lv_obj_set_size(t.ph, kTilePx, kTilePx);
    lv_obj_set_style_bg_color(t.ph, lv_color_hex(0x1c2330), 0);
    lv_obj_set_style_bg_opa(t.ph, LV_OPA_COVER, 0);
    lv_obj_set_style_border_opa(t.ph, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(t.ph, 0, 0);
    lv_obj_clear_flag(t.ph, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(t.ph, LV_OBJ_FLAG_HIDDEN);
    t.src[0] = 0;
    t.has_real_tile = false;
    t.fast_hidden_img = false;
  }
  s_tile_slots_ready = true;
}

static void set_fast_pan_tile_mode(bool enable) {
  // Disabled: this mode swaps real tiles for dark placeholders while
  // dragging. On this target it can make the map appear black during
  // pan/zoom bursts and under decode/cache pressure.
  (void)enable;
  return;

  for (int i = 0; i < kMaxTileSlots; i++) {
    TileSlot& t = s_tile_slots[i];
    if (!t.img || !t.ph) continue;
    if (enable) {
      if (t.has_real_tile && !lv_obj_has_flag(t.img, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_set_pos(t.ph, lv_obj_get_x(t.img), lv_obj_get_y(t.img));
        lv_obj_clear_flag(t.ph, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(t.img, LV_OBJ_FLAG_HIDDEN);
        t.fast_hidden_img = true;
      }
    } else {
      if (t.fast_hidden_img && t.has_real_tile) {
        lv_obj_add_flag(t.ph, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(t.img, LV_OBJ_FLAG_HIDDEN);
      }
      t.fast_hidden_img = false;
    }
  }
}

static void ensure_marker_slots() {
  if (!s.pan_layer || s_marker_slots_ready) return;
  for (int i = 0; i < kMaxMarkerSlots; i++) {
    MarkerSlot& m = s_marker_slots[i];
    m.dot = lv_obj_create(s.pan_layer);
    lv_obj_set_size(m.dot, 8, 8);
    // LV_RADIUS_CIRCLE adapts to whatever size rebuild_map sets later,
    // so both repeater (8 px) and companion (6 px) dots render as circles.
    lv_obj_set_style_radius(m.dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(m.dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_opa(m.dot, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_opa(m.dot, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(m.dot, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(m.dot, LV_OBJ_FLAG_HIDDEN);

    m.lbl = lv_label_create(s.pan_layer);
    // Gray pill behind the name so it reads clearly against any tile.
    lv_obj_set_style_text_color(m.lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(m.lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_bg_color(m.lbl, lv_color_hex(0x5A5A5A), 0);
    lv_obj_set_style_bg_opa(m.lbl, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(m.lbl, 3, 0);
    lv_obj_set_style_pad_hor(m.lbl, 3, 0);
    lv_obj_set_style_pad_ver(m.lbl, 1, 0);
    lv_obj_clear_flag(m.lbl, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(m.lbl, LV_OBJ_FLAG_HIDDEN);
    m.name[0] = 0;
    m.active_dot = false;
    m.active_lbl = false;
  }
  s_marker_slots_ready = true;
}

static void apply_pending_drag_translation() {
  if (!s.pan_layer) return;
  if (s_pending_dx == 0 && s_pending_dy == 0) return;
  lv_obj_set_pos(s.pan_layer, lv_obj_get_x(s.pan_layer) + s_pending_dx, lv_obj_get_y(s.pan_layer) + s_pending_dy);
  s_pending_dx = 0;
  s_pending_dy = 0;
}

static bool should_rebuild_after_drag() {
  if (!s.content || !s.pan_layer) return true;
  const int vw = lv_obj_get_width(s.content);
  const int vh = lv_obj_get_height(s.content);
  const int x = lv_obj_get_x(s.pan_layer);
  const int y = lv_obj_get_y(s.pan_layer);
  const int w = lv_obj_get_width(s.pan_layer);
  const int h = lv_obj_get_height(s.pan_layer);
  if (w <= 0 || h <= 0) return true;

  // If the existing composed layer still covers the viewport with some
  // safety margin, skip a costly rebuild and keep things smooth.
  const bool cover_x = (x <= kRebuildEdgeMarginPx) &&
                       (x + w >= vw - kRebuildEdgeMarginPx);
  const bool cover_y = (y <= kRebuildEdgeMarginPx) &&
                       (y + h >= vh - kRebuildEdgeMarginPx);
  return !(cover_x && cover_y);
}

static int rebuild_map(bool include_markers) {
  if (!s.pan_layer) return 0;
  ensure_tile_slots();
  ensure_marker_slots();
  for (int i = 0; i < kMaxMarkerSlots; i++) {
    MarkerSlot& m = s_marker_slots[i];
    m.active_dot = false;
    m.active_lbl = false;
    if (m.dot) lv_obj_add_flag(m.dot, LV_OBJ_FLAG_HIDDEN);
    if (m.lbl) lv_obj_add_flag(m.lbl, LV_OBJ_FLAG_HIDDEN);
  }

  if (!sd_is_mounted()) {
    for (int i = 0; i < kMaxTileSlots; i++) {
      if (s_tile_slots[i].img) lv_obj_add_flag(s_tile_slots[i].img, LV_OBJ_FLAG_HIDDEN);
      if (s_tile_slots[i].ph)  lv_obj_add_flag(s_tile_slots[i].ph, LV_OBJ_FLAG_HIDDEN);
    }
    if (s.zoom_label) lv_label_set_text(s.zoom_label, "No SD card");
    return 0;
  }

  const int view_w = SCR_W;
  const int view_h = SCR_H - STATUS_H - TAB_H;

  double cwx = 0.0, cwy = 0.0;
  latlon_to_world(s.center_lat, s.center_lon, s.zoom, &cwx, &cwy);
  const double origin_x = cwx - view_w / 2.0;
  const double origin_y = cwy - view_h / 2.0;

  const int vis_x_start = (int)floor(origin_x / kTilePx);
  const int vis_y_start = (int)floor(origin_y / kTilePx);
  const int vis_x_end = (int)floor((origin_x + view_w - 1) / kTilePx);
  const int vis_y_end = (int)floor((origin_y + view_h - 1) / kTilePx);
  const int vis_cols = vis_x_end - vis_x_start + 1;
  const int vis_rows = vis_y_end - vis_y_start + 1;

  // Adaptive pad: keep a hot ring when it fits in slot budget.
  int tile_pad = kDesiredTilePad;
  while (tile_pad > 0) {
    const int cols_try = vis_cols + 2 * tile_pad;
    const int rows_try = vis_rows + 2 * tile_pad;
    if (cols_try * rows_try <= kMaxTileSlots) break;
    tile_pad--;
  }

  const int x_start = vis_x_start - tile_pad;
  const int y_start = vis_y_start - tile_pad;
  const int x_end = vis_x_end + tile_pad;
  const int y_end = vis_y_end + tile_pad;

  const int cols = x_end - x_start + 1;
  const int rows = y_end - y_start + 1;
  const int pan_w = cols * kTilePx;
  const int pan_h = rows * kTilePx;
  const int base_x = (int)round(x_start * kTilePx - origin_x);
  const int base_y = (int)round(y_start * kTilePx - origin_y);

  lv_obj_set_size(s.pan_layer, pan_w, pan_h);
  lv_obj_set_pos(s.pan_layer, base_x + s.drag_dx, base_y + s.drag_dy);

  const int tile_max = 1 << s.zoom;
  int loaded_tiles = 0;
  int total_tiles = 0;
  int slot_idx = 0;

  for (int r = 0; r < rows; r++) {
    for (int c = 0; c < cols; c++) {
      const int tx = x_start + c;
      const int ty = y_start + r;
      if (tx < 0 || ty < 0 || tx >= tile_max || ty >= tile_max) continue;
      total_tiles++;

      const int sx = c * kTilePx;
      const int sy = r * kTilePx;

      if (slot_idx >= kMaxTileSlots) continue;
      TileSlot& t = s_tile_slots[slot_idx++];

      char vfs[112];
      if (pick_tile_source(s.zoom, tx, ty, vfs, sizeof(vfs))) {
        if (strncmp(t.src, vfs, sizeof(t.src)) != 0) {
          strncpy(t.src, vfs, sizeof(t.src) - 1);
          t.src[sizeof(t.src) - 1] = 0;
          lv_img_set_src(t.img, t.src);
        }
        lv_obj_set_pos(t.img, sx, sy);
        lv_obj_clear_flag(t.img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(t.ph, LV_OBJ_FLAG_HIDDEN);
        loaded_tiles++;
        t.has_real_tile = true;
        t.fast_hidden_img = false;
      } else {
        lv_obj_set_pos(t.ph, sx, sy);
        lv_obj_clear_flag(t.ph, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(t.img, LV_OBJ_FLAG_HIDDEN);
        t.has_real_tile = false;
        t.fast_hidden_img = false;
      }
    }
  }

  for (int i = slot_idx; i < kMaxTileSlots; i++) {
    if (s_tile_slots[i].img) lv_obj_add_flag(s_tile_slots[i].img, LV_OBJ_FLAG_HIDDEN);
    if (s_tile_slots[i].ph) lv_obj_add_flag(s_tile_slots[i].ph, LV_OBJ_FLAG_HIDDEN);
    s_tile_slots[i].has_real_tile = false;
    s_tile_slots[i].fast_hidden_img = false;
  }

  int drawn_pins = 0;
  int hidden_pins = 0;
  const bool markers_allowed = (s.zoom >= kMarkerMinZoom);

  if (include_markers && markers_allowed && g_mesh) {
#if defined(ESP32)
    bool locked = true;
    if (g_mesh_mutex) locked = (xSemaphoreTake(g_mesh_mutex, pdMS_TO_TICKS(10)) == pdTRUE);
    if (!locked) {
      update_overlay_labels(loaded_tiles, total_tiles, 0, 0);
      return loaded_tiles;
    }
#endif

    const double pan_origin_wx = (double)x_start * kTilePx;
    const double pan_origin_wy = (double)y_start * kTilePx;

    const int marker_cell = (s.zoom <= 9) ? 42 : (s.zoom <= 11) ? 34 : 28;
    const int max_cols = 64;
    const int max_rows = 40;
    uint8_t used[max_cols * max_rows];
    memset(used, 0, sizeof(used));
    const int cell_cols = ((view_w + marker_cell - 1) / marker_cell) + 2;
    const int cell_rows = ((view_h + marker_cell - 1) / marker_cell) + 2;

    const int marker_cap = (s.zoom <= 9) ? 18 : (s.zoom <= 11) ? 26 : 34;
    const bool show_repeater_names = (s.zoom == 11 || s.zoom == 12);
    int name_budget = 22;

    ContactsIterator it;
    ContactInfo ci;
    while (it.hasNext(g_mesh, ci)) {
      const bool is_companion = (ci.type == ADV_TYPE_CHAT);
      const bool is_repeater = (ci.type == ADV_TYPE_REPEATER);
      if (!is_companion && !is_repeater) continue;
      if (s.filter == FILTER_COMPANIONS && !is_companion) continue;
      if (s.filter == FILTER_REPEATERS && !is_repeater) continue;
      if (ci.gps_lat == 0 && ci.gps_lon == 0) continue;

      const double lat = (double)ci.gps_lat / 1.0e6;
      const double lon = (double)ci.gps_lon / 1.0e6;
      double mwx = 0.0, mwy = 0.0;
      latlon_to_world(lat, lon, s.zoom, &mwx, &mwy);

      const int mx = (int)round(mwx - pan_origin_wx);
      const int my = (int)round(mwy - pan_origin_wy);

      const int screen_x = lv_obj_get_x(s.pan_layer) + mx;
      const int screen_y = lv_obj_get_y(s.pan_layer) + my;
      if (screen_x < -20 || screen_y < -20 || screen_x > view_w + 20 || screen_y > view_h + 20) continue;

      const int cx = (screen_x + marker_cell) / marker_cell;
      const int cy = (screen_y + marker_cell) / marker_cell;
      if (cx < 0 || cy < 0 || cx >= cell_cols || cy >= cell_rows) continue;

      // Companion markers are throttled by cell-dedup + a soft cap so a
      // dense cluster doesn't paint over itself. Repeater markers are
      // infrastructure (and the user expects them visible at all times),
      // so they bypass both throttles — they're still bounded by the hard
      // slot limit (kMaxMarkerSlots) below. This fixes red markers
      // "disappearing at random" between rebuilds: previously the marker_cap
      // (18-34) was reached and which markers got dropped depended on
      // ContactsIterator order, which isn't stable across rebuilds.
      if (!is_repeater) {
        if (cx < max_cols && cy < max_rows) {
          const int idx = cy * max_cols + cx;
          if (used[idx]) continue;
          used[idx] = 1;
        }

        if (drawn_pins >= marker_cap) {
          hidden_pins++;
          continue;
        }
      }

      if (drawn_pins >= kMaxMarkerSlots) {
        hidden_pins++;
        continue;
      }

      MarkerSlot& ms = s_marker_slots[drawn_pins];
      const int dot_sz = is_repeater ? 8 : 6;
      lv_obj_set_size(ms.dot, dot_sz, dot_sz);
      lv_obj_set_pos(ms.dot, mx - dot_sz / 2, my - dot_sz / 2);
      lv_obj_set_style_bg_color(ms.dot, is_repeater ? lv_color_hex(0xFF3B30) : lv_color_hex(0x35A4FF), 0);
      lv_obj_clear_flag(ms.dot, LV_OBJ_FLAG_HIDDEN);
      ms.active_dot = true;
      ms.active_lbl = false;
      drawn_pins++;

      if (is_repeater && show_repeater_names && name_budget > 0 &&
          ci.name[0] && memchr(ci.name, 0, sizeof(ci.name)) != nullptr) {
        strncpy(ms.name, ci.name, sizeof(ms.name) - 1);
        ms.name[sizeof(ms.name) - 1] = 0;
        lv_label_set_text_static(ms.lbl, ms.name);
        lv_obj_set_pos(ms.lbl, mx + dot_sz / 2 + 3, my - 7);
        lv_obj_clear_flag(ms.lbl, LV_OBJ_FLAG_HIDDEN);
        ms.active_lbl = true;
        name_budget--;
      }
    }

#if defined(ESP32)
    if (g_mesh_mutex) xSemaphoreGive(g_mesh_mutex);
#endif
  }

  if (!markers_allowed) {
    update_overlay_labels(loaded_tiles, total_tiles, 0, 0);
  } else {
    update_overlay_labels(loaded_tiles, total_tiles, drawn_pins, hidden_pins);
  }
  return loaded_tiles;
}

static bool ensure_nonempty_map_view(bool include_markers) {
  int loaded = rebuild_map(include_markers);
  if (loaded > 0) return true;

  // If current zoom has no coverage here, step down until we find tiles.
  const int original_zoom = s.zoom;
  for (int z = s.zoom - 1; z >= kMinZoom; --z) {
    s.zoom = z;
    loaded = rebuild_map(include_markers);
    if (loaded > 0) {
      map_viewport_mark_dirty();
      return true;
    }
  }

  // Restore zoom if we still found nothing.
  s.zoom = original_zoom;
  (void)rebuild_map(include_markers);
  return false;
}

static void marker_restore_timer_cb(lv_timer_t*) {
  if (!ui_mapscreen || lv_scr_act() != ui_mapscreen) return;
  (void)ensure_nonempty_map_view(true);
  s_markers_hidden = false;
  if (s_marker_restore_timer) lv_timer_pause(s_marker_restore_timer);
}

static void content_pressing_cb(lv_event_t*) {
  lv_indev_t* indev = lv_indev_get_act();
  if (!indev) return;
  lv_point_t v;
  lv_indev_get_vect(indev, &v);
  if (v.x == 0 && v.y == 0) return;

  if (!s_markers_hidden) {
    if (s_marker_restore_timer) lv_timer_pause(s_marker_restore_timer);
    set_marker_visibility(false);
    s_markers_hidden = true;
  }
  set_fast_pan_tile_mode(true);

  s.drag_dx += v.x;
  s.drag_dy += v.y;
  s_pending_dx += v.x;
  s_pending_dy += v.y;

  if (kDragApplyMs == 0) {
    apply_pending_drag_translation();
  } else {
    const uint32_t now = millis();
    if ((uint32_t)(now - s_last_drag_apply_ms) >= kDragApplyMs) {
      apply_pending_drag_translation();
      s_last_drag_apply_ms = now;
    }
  }
}

static void content_released_cb(lv_event_t*) {
  if (s.drag_dx == 0 && s.drag_dy == 0) {
    set_fast_pan_tile_mode(false);
    if (s_markers_hidden) {
      set_marker_visibility(true);
      s_markers_hidden = false;
    }
    s_pending_dx = 0;
    s_pending_dy = 0;
    return;
  }

  apply_pending_drag_translation();

  const double old_lat = s.center_lat;
  const double old_lon = s.center_lon;

  double cwx = 0.0, cwy = 0.0;
  latlon_to_world(s.center_lat, s.center_lon, s.zoom, &cwx, &cwy);
  cwx -= s.drag_dx;
  cwy -= s.drag_dy;

  const double ws = world_size_px(s.zoom);
  if (cwx < 0) cwx = 0;
  if (cwx > ws - 1) cwx = ws - 1;
  if (cwy < 0) cwy = 0;
  if (cwy > ws - 1) cwy = ws - 1;

  world_to_latlon(cwx, cwy, s.zoom, &s.center_lat, &s.center_lon);

  s.drag_dx = 0;
  s.drag_dy = 0;
  s_pending_dx = 0;
  s_pending_dy = 0;

  // Quick visual settle:
  // - If current composed layer still covers viewport, avoid rebuild to keep pan snappy.
  // - Rebuild only when edge coverage is exhausted.
  if (should_rebuild_after_drag()) {
    const int loaded = rebuild_map(false);
    if (loaded <= 0) {
      // Don't commit to an empty/black viewport. Roll back to last valid center.
      s.center_lat = old_lat;
      s.center_lon = old_lon;
      set_fast_pan_tile_mode(false);
      (void)ensure_nonempty_map_view(true);
      set_marker_visibility(true);
      s_markers_hidden = false;
      return;
    }
    set_fast_pan_tile_mode(false);
    if (s_marker_restore_timer) {
      lv_timer_set_period(s_marker_restore_timer, kMarkerRestoreDelayMs);
      lv_timer_set_repeat_count(s_marker_restore_timer, 1);
      lv_timer_resume(s_marker_restore_timer);
      lv_timer_reset(s_marker_restore_timer);
    } else {
      (void)ensure_nonempty_map_view(true);
      s_markers_hidden = false;
    }
  } else {
    set_fast_pan_tile_mode(false);
    set_marker_visibility(true);
    s_markers_hidden = false;
  }
  map_viewport_mark_dirty();
}

static void zoom_by(int dz) {
  const int z = s.zoom + dz;
  if (z < kMinZoom || z > kMaxZoom) return;
  s.zoom = z;
  s.drag_dx = s.drag_dy = 0;
  s_pending_dx = s_pending_dy = 0;
  (void)rebuild_map(true);
  map_viewport_mark_dirty();
}

static void cb_zoom_in(lv_event_t*) { zoom_by(+1); }
static void cb_zoom_out(lv_event_t*) { zoom_by(-1); }

static void cb_mode_cycle(lv_event_t*) {
  int next = (int)s.filter + 1;
  if (next > FILTER_REPEATERS) next = FILTER_ALL;
  s.filter = (MarkerFilter)next;
  (void)ensure_nonempty_map_view(true);
  map_viewport_mark_dirty();
}

static lv_obj_t* make_btn(lv_obj_t* parent, const char* text, lv_event_cb_t cb, int w, int h, const lv_font_t* font) {
  lv_obj_t* btn = lv_btn_create(parent);
  lv_obj_set_size(btn, w, h);
  lv_obj_set_style_radius(btn, 0, 0);
  lv_obj_set_style_bg_color(btn, lv_color_hex(TH_SURFACE), 0);
  lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
  lv_obj_set_style_border_opa(btn, LV_OPA_TRANSP, 0);
  lv_obj_set_style_shadow_opa(btn, LV_OPA_TRANSP, 0);
  lv_obj_set_style_pad_all(btn, 0, 0);
  lv_obj_add_flag(btn, LV_OBJ_FLAG_FLOATING);
  lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);

  lv_obj_t* lbl = lv_label_create(btn);
  lv_label_set_text(lbl, text);
  lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
  lv_obj_set_style_text_font(lbl, font, 0);
  lv_obj_center(lbl);
  return btn;
}

} // namespace

void map_viewport_save_if_dirty_flush() {
  map_viewport_save_now();
}

void ui_mapscreen_screen_init(void) {
  if (ui_mapscreen) return;

  s.center_lat = kInitLat;
  s.center_lon = kInitLon;
  s.zoom = kInitZoom;
  s.filter = FILTER_ALL;
  s.drag_dx = s.drag_dy = 0;
  s_pending_dx = s_pending_dy = 0;
  s_last_drag_apply_ms = millis();
  (void)map_viewport_load();

  lv_img_cache_set_size(kImgCacheEntries);
  if (!s_marker_restore_timer) {
    s_marker_restore_timer = lv_timer_create(marker_restore_timer_cb, kMarkerRestoreDelayMs, nullptr);
    if (s_marker_restore_timer) lv_timer_pause(s_marker_restore_timer);
  }

  ui_mapscreen = lv_obj_create(NULL);
  lv_obj_clear_flag(ui_mapscreen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(ui_mapscreen, lv_color_hex(TH_BG), 0);
  lv_obj_set_style_bg_opa(ui_mapscreen, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(ui_mapscreen, 0, 0);

  lv_obj_t* hdr = lv_obj_create(ui_mapscreen);
  lv_obj_set_size(hdr, SCR_W, STATUS_H);
  lv_obj_set_pos(hdr, 0, 0);
  lv_obj_set_style_bg_color(hdr, lv_color_hex(TH_SURFACE), 0);
  lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(hdr, 1, 0);
  lv_obj_set_style_border_color(hdr, lv_color_hex(TH_SEPARATOR), 0);
  lv_obj_set_style_border_side(hdr, LV_BORDER_SIDE_BOTTOM, 0);
  lv_obj_set_style_radius(hdr, 0, 0);
  lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* title = lv_label_create(hdr);
  lv_label_set_text(title, LV_SYMBOL_GPS "  Maps");
  lv_obj_set_style_text_color(title, lv_color_hex(TH_TEXT), 0);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
  lv_obj_center(title);

  s.content = lv_obj_create(ui_mapscreen);
  lv_obj_set_size(s.content, SCR_W, SCR_H - STATUS_H - TAB_H);
  lv_obj_set_pos(s.content, 0, STATUS_H);
  lv_obj_set_style_bg_color(s.content, lv_color_hex(0x16202B), 0);
  lv_obj_set_style_bg_opa(s.content, LV_OPA_COVER, 0);
  lv_obj_set_style_border_opa(s.content, LV_OPA_TRANSP, 0);
  lv_obj_set_style_radius(s.content, 0, 0);
  lv_obj_set_style_pad_all(s.content, 0, 0);
  lv_obj_clear_flag(s.content, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(s.content, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(s.content, content_pressing_cb, LV_EVENT_PRESSING, nullptr);
  lv_obj_add_event_cb(s.content, content_released_cb, LV_EVENT_RELEASED, nullptr);

  s.pan_layer = lv_obj_create(s.content);
  lv_obj_set_style_bg_opa(s.pan_layer, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_opa(s.pan_layer, LV_OPA_TRANSP, 0);
  lv_obj_set_style_pad_all(s.pan_layer, 0, 0);
  lv_obj_set_style_radius(s.pan_layer, 0, 0);
  lv_obj_clear_flag(s.pan_layer, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

  s.zoom_label = lv_label_create(s.content);
  lv_obj_set_style_text_color(s.zoom_label, lv_color_white(), 0);
  lv_obj_set_style_text_font(s.zoom_label, &lv_font_montserrat_12, 0);
  lv_obj_set_style_bg_color(s.zoom_label, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(s.zoom_label, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_hor(s.zoom_label, 6, 0);
  lv_obj_set_style_pad_ver(s.zoom_label, 4, 0);
  lv_obj_align(s.zoom_label, LV_ALIGN_TOP_LEFT, 8, 8);
  lv_obj_add_flag(s.zoom_label, LV_OBJ_FLAG_FLOATING);

  s.mode_label = lv_label_create(s.content);
  lv_obj_set_style_text_color(s.mode_label, lv_color_white(), 0);
  lv_obj_set_style_text_font(s.mode_label, &lv_font_montserrat_12, 0);
  lv_obj_set_style_bg_color(s.mode_label, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(s.mode_label, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_hor(s.mode_label, 6, 0);
  lv_obj_set_style_pad_ver(s.mode_label, 3, 0);
  lv_obj_align(s.mode_label, LV_ALIGN_TOP_LEFT, 8, 36);
  lv_obj_add_flag(s.mode_label, LV_OBJ_FLAG_FLOATING);

  lv_obj_t* btn_in = make_btn(s.content, "+", cb_zoom_in, 46, 46, &lv_font_montserrat_24);
  lv_obj_t* btn_out = make_btn(s.content, "-", cb_zoom_out, 46, 46, &lv_font_montserrat_24);
  lv_obj_t* btn_mode = make_btn(s.content, "Mode", cb_mode_cycle, 68, 30, &lv_font_montserrat_14);
  lv_obj_align(btn_in, LV_ALIGN_BOTTOM_RIGHT, -8, -60);
  lv_obj_align(btn_out, LV_ALIGN_BOTTOM_RIGHT, -8, -8);
  lv_obj_align(btn_mode, LV_ALIGN_TOP_RIGHT, -8, 8);

  ui_tabbar_create(ui_mapscreen, 2);

  (void)ensure_nonempty_map_view(true);
  if (!sd_is_mounted()) serialmon_append("maps: no SD mounted");
}

void ui_mapscreen_screen_destroy(void) {
  if (!ui_mapscreen) return;
  if (s_marker_restore_timer) lv_timer_pause(s_marker_restore_timer);
  lv_obj_del(ui_mapscreen);
  ui_mapscreen = nullptr;
  s.content = nullptr;
  s.pan_layer = nullptr;
  s.zoom_label = nullptr;
  s.mode_label = nullptr;
  s_markers_hidden = false;
  s_pending_dx = 0;
  s_pending_dy = 0;
  s_tile_slots_ready = false;
  s_marker_slots_ready = false;
  for (int i = 0; i < kMaxTileSlots; i++) {
    s_tile_slots[i].img = nullptr;
    s_tile_slots[i].ph = nullptr;
    s_tile_slots[i].src[0] = 0;
  }
  for (int i = 0; i < kMaxMarkerSlots; i++) {
    s_marker_slots[i].dot = nullptr;
    s_marker_slots[i].lbl = nullptr;
    s_marker_slots[i].name[0] = 0;
    s_marker_slots[i].active_dot = false;
    s_marker_slots[i].active_lbl = false;
  }
}
