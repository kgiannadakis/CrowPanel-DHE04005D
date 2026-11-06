// ============================================================
// map_view.cpp — Simplified high-performance map screen
// ------------------------------------------------------------
// Goals:
//  - Fast/smooth panning over visual quality
//  - Show companions + repeaters as markers
//  - Optional repeater names at any zoom
// ============================================================

#include "map_view.h"
#include "neighbour_map_policy.h"
#include "advert_location_cache.h"
#include "app_globals.h"
#include "ui_theme.h"
#include "ui_tabbar.h"
#include "ui_battery.h"
#include "ui_homescreen.h"
#include "sd_storage.h"
#include "utils.h"

#include <Preferences.h>
#include <esp_heap_caps.h>
#include <new>
#include <helpers/AdvertDataHelpers.h>
#include <helpers/BaseChatMesh.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
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
constexpr int kRebuildEdgeMarginPx = 24;
// More tile slots so we can keep a prefetch ring around the viewport.
constexpr int kMaxTileSlots = 40;
constexpr int kDesiredTilePad = 1;
constexpr int kMaxMapMarkers = MAX_CONTACTS;

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
  bool show_names = false;

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

struct MapMarker {
  int32_t lat, lon;
  uint8_t type;
  char name[32];
  lv_point_t name_size;
  double world_x, world_y;
  int screen_x, screen_y;
  int8_t snr_x4;
};

MapState s;
struct NeighbourReply {
  bool waiting = false, ready = false;
  uint32_t tag = 0, started = 0, timeout = 60000;
  uint8_t source[32] = {}, bytes[188] = {};
  size_t len = 0;
};
portMUX_TYPE s_neighbour_mux = portMUX_INITIALIZER_UNLOCKED;
NeighbourReply s_neighbour_reply;
MapMarker s_neighbour_points[20];
int s_neighbour_count = 0;
bool s_neighbour_active = false;
char s_neighbour_name[32] = {};
lv_obj_t* s_neighbour_label = nullptr;
lv_obj_t* s_neighbour_clear = nullptr;
enum class PositionStage { Idle, NeedLat, WaitLat, NeedLon, WaitLon, Ready, Failed, Unset };
struct RepeaterPosition {
  PositionStage stage = PositionStage::Idle;
  uint8_t nonce = 0;
  uint32_t started = 0, timeout = 60000;
  double lat = 0, lon = 0;
};
RepeaterPosition s_remote_position; // protected by s_neighbour_mux
uint8_t s_position_nonce = 0;
MapMarker s_neighbour_origin;
bool s_neighbour_origin_valid = false;
int s_neighbour_reported = -1;
NeighbourReply s_neighbour_snapshot;
bool s_neighbour_snapshot_valid = false;
int s_neighbour_unknown = 0, s_neighbour_no_gps = 0, s_neighbour_ambiguous = 0, s_neighbour_other = 0;
using AdvertCache = AdvertLocations::Cache<MAX_CONTACTS>;
AdvertCache* s_advert_locations = nullptr;
portMUX_TYPE s_advert_location_mux = portMUX_INITIALIZER_UNLOCKED;
int s_advert_mapped = 0;

struct DiagnosticRepeater {
  uint8_t key[8];
  char name[32];
  int32_t lat, lon;
  uint32_t advert;
  bool cached, neighbour;
};
struct DiagnosticMatch {
  uint8_t key[4];
  char name[32];
  char screen_name[32];
  unsigned screen_matches;
  int32_t lat, lon;
  uint32_t heard_seconds_ago;
  int8_t snr_x4;
  unsigned saved_matches, advert_matches;
  const char* reason;
  bool from_advert;
};
struct NeighbourDiagnostics {
  DiagnosticRepeater inventory[MAX_CONTACTS * 2];
  DiagnosticMatch matches[20];
  uint8_t raw[188];
  size_t raw_len, raw_cursor;
  unsigned inventory_count, match_count, inventory_cursor, match_cursor;
  unsigned saved_repeaters, saved_gps, cached_adverts, screen_entries;
  bool ready, header_printed;
};
NeighbourDiagnostics* s_diagnostics = nullptr; // PSRAM, one manual request only.
lv_timer_t* s_diagnostic_timer = nullptr;
bool s_diagnostic_render_pending = false;
uint32_t s_diagnostic_render_requested_ms = 0;
struct NeighbourDrawDiagnostics {
  bool seen;
  unsigned attempted, drawn, clipped, coincident;
} s_neighbour_draw = {};

static void request_draw_diagnostics() {
  s_neighbour_draw.seen = false;
  s_diagnostic_render_pending = true;
  s_diagnostic_render_requested_ms = millis();
  if (s_diagnostic_timer) lv_timer_resume(s_diagnostic_timer);
}

static void clear_neighbour_diagnostics() {
  if (s_diagnostics) heap_caps_free(s_diagnostics);
  s_diagnostics = nullptr;
  s_diagnostic_render_pending = false;
  if (s_diagnostic_timer) lv_timer_pause(s_diagnostic_timer);
}

static bool matches_neighbour(const uint8_t* key, const NeighbourReply& reply) {
  const int count = NeighbourMap::entryCount(reply.len, reply.bytes[6] | (reply.bytes[7] << 8));
  for (int i = 0; i < count; ++i)
    if (!memcmp(key, reply.bytes + 8 + i * 9, 4)) return true;
  return false;
}

// Caller holds mesh mutex; only copies bounded data, never prints or allocates.
static void snapshot_detected_repeaters(const NeighbourReply& reply) {
  if (!s_diagnostics) return;
  s_diagnostics->raw_len = LV_MIN(reply.len, sizeof(s_diagnostics->raw));
  memcpy(s_diagnostics->raw, reply.bytes, s_diagnostics->raw_len);
  s_diagnostics->screen_entries = LV_MIN(g_repeater_count, MAX_DD_ENTRIES);
  ContactInfo contact;
  auto iterator = g_mesh->startContactsIterator();
  while (iterator.hasNext(g_mesh, contact)) {
    if (contact.type != ADV_TYPE_REPEATER) continue;
    ++s_diagnostics->saved_repeaters;
    if (NeighbourMap::validPosition(contact.gps_lat / 1.e6, contact.gps_lon / 1.e6))
      ++s_diagnostics->saved_gps;
    if (s_diagnostics->inventory_count >= MAX_CONTACTS * 2) continue;
    auto& row = s_diagnostics->inventory[s_diagnostics->inventory_count++];
    memcpy(row.key, contact.id.pub_key, sizeof(row.key));
    memcpy(row.name, contact.name, sizeof(row.name));
    row.name[31] = 0;
    row.lat = contact.gps_lat; row.lon = contact.gps_lon;
    row.advert = contact.last_advert_timestamp;
    row.neighbour = matches_neighbour(contact.id.pub_key, reply);
  }
  portENTER_CRITICAL(&s_advert_location_mux);
  if (s_advert_locations) {
    s_diagnostics->cached_adverts = s_advert_locations->size();
    AdvertLocations::Location record;
    for (size_t i = 0; s_advert_locations->get(i, record); ++i) {
      if (s_diagnostics->inventory_count >= MAX_CONTACTS * 2) break;
      auto& row = s_diagnostics->inventory[s_diagnostics->inventory_count++];
      memcpy(row.key, record.key, sizeof(row.key));
      memcpy(row.name, record.name, sizeof(row.name));
      row.name[31] = 0;
      row.lat = record.lat; row.lon = record.lon; row.advert = record.timestamp;
      row.cached = true;
      row.neighbour = matches_neighbour(record.key, reply);
    }
  }
  portEXIT_CRITICAL(&s_advert_location_mux);
}

static void neighbour_diagnostic_timer_cb(lv_timer_t*) {
  if (!s_neighbour_active || !ui_mapscreen || lv_scr_act() != ui_mapscreen) return;
  if (s_diagnostics && s_diagnostics->ready) {
    auto& dump = *s_diagnostics;
    if (!dump.header_printed) {
      Serial.printf("[neighbour-diag] inventory: saved_repeaters=%u valid_GPS=%u cached_location_adverts=%u screen_entries=%u; comparing 4-byte prefixes; coordinates are degrees\n",
                    dump.saved_repeaters, dump.saved_gps, dump.cached_adverts, dump.screen_entries);
      dump.header_printed = true;
    }
    // Two bounded rows per tick, with no mesh/cache mutex held.
    for (unsigned batch = 0; batch < 2; ++batch) {
      if (dump.match_cursor < dump.match_count) {
        const auto& row = dump.matches[dump.match_cursor++];
        Serial.printf("[neighbour-diag] compare %02X%02X%02X%02X name=\"%.31s\" saved_matches=%u advert_matches=%u GPS=%.6f,%.6f source=%s result=%s heard_ago=%lus SNR=%+.2f dB screen_matches=%u screen_name=\"%.31s\"\n",
                      row.key[0], row.key[1], row.key[2], row.key[3], row.name,
                      row.saved_matches, row.advert_matches, row.lat / 1.e6, row.lon / 1.e6,
                      row.from_advert ? "received-advert" : "saved-contact", row.reason,
                      (unsigned long)row.heard_seconds_ago, row.snr_x4 / 4.0f,
                      row.screen_matches, row.screen_name);
      } else if (dump.raw_cursor < dump.raw_len) {
        // Include padding and exact offsets so a different remote format is detectable.
        char hex[16 * 2 + 1];
        const size_t count = LV_MIN((size_t)16, dump.raw_len - dump.raw_cursor);
        static const char digits[] = "0123456789ABCDEF";
        for (size_t i = 0; i < count; ++i) {
          const uint8_t value = dump.raw[dump.raw_cursor + i];
          hex[2 * i] = digits[value >> 4];
          hex[2 * i + 1] = digits[value & 15];
        }
        hex[2 * count] = 0;
        Serial.printf("[neighbour-diag] raw offset=%u bytes=%s\n",
                      (unsigned)dump.raw_cursor, hex);
        dump.raw_cursor += count;
      } else if (dump.inventory_cursor < dump.inventory_count) {
        const auto& row = dump.inventory[dump.inventory_cursor++];
        Serial.printf("[neighbour-diag] detected %s key=%02X%02X%02X%02X%02X%02X%02X%02X name=\"%.31s\" GPS=%.6f,%.6f valid=%u advert_ts=%lu in_neighbours=%s\n",
                      row.cached ? "advert-cache" : "saved-repeater", row.key[0], row.key[1], row.key[2], row.key[3],
                      row.key[4], row.key[5], row.key[6], row.key[7], row.name,
                      row.lat / 1.e6, row.lon / 1.e6,
                      NeighbourMap::validPosition(row.lat / 1.e6, row.lon / 1.e6) ? 1u : 0u,
                      (unsigned long)row.advert, row.neighbour ? "yes" : "no");
      } else {
        Serial.println("[neighbour-diag] comparison complete; no contacts added or changed");
        heap_caps_free(s_diagnostics);
        s_diagnostics = nullptr;
        break;
      }
    }
  }
  if (s_diagnostic_render_pending && s_neighbour_draw.seen) {
    Serial.printf("[neighbour-diag] draw: origin=%s mapped=%d attempted=%u drawn=%u clipped=%u coincident=%u zoom=%d viewport=%dx%d; black links\n",
                  s_neighbour_origin_valid ? "valid" : "MISSING", s_neighbour_count,
                  s_neighbour_draw.attempted, s_neighbour_draw.drawn, s_neighbour_draw.clipped,
                  s_neighbour_draw.coincident, s.zoom,
                  s.content ? lv_obj_get_width(s.content) : 0,
                  s.content ? lv_obj_get_height(s.content) : 0);
    s_diagnostic_render_pending = false;
  } else if (s_diagnostic_render_pending &&
             (uint32_t)(millis() - s_diagnostic_render_requested_ms) > 3000) {
    Serial.printf("[neighbour-diag] draw: marker draw callback NOT SEEN for 3 seconds; origin=%s mapped=%d\n",
                  s_neighbour_origin_valid ? "valid" : "MISSING", s_neighbour_count);
    s_diagnostic_render_pending = false;
  }
  if (!s_diagnostics && !s_diagnostic_render_pending && s_diagnostic_timer)
    lv_timer_pause(s_diagnostic_timer);
}

int s_pending_dx = 0;
int s_pending_dy = 0;
uint32_t s_last_drag_apply_ms = 0;
lv_timer_t* s_marker_refresh_timer = nullptr;
TileSlot s_tile_slots[kMaxTileSlots];
bool s_tile_slots_ready = false;
MapMarker s_marker_slots[kMaxMapMarkers];
bool s_marker_slots_ready = false;
lv_obj_t* s_marker_layer = nullptr;
int s_marker_count = 0;
int s_gesture_travel = 0;
bool s_dragging = false;
bool s_projection_valid = false;
bool s_projected_self_valid = false;
double s_projected_lat = 0, s_projected_lon = 0;
double s_projected_self_lat = 0, s_projected_self_lon = 0;
int s_projected_zoom = 0, s_projected_width = 0, s_projected_height = 0;
int s_self_x = 0, s_self_y = 0;

struct MarkerCluster {
  int x, y, count, first;
  int64_t sum_x, sum_y;
  bool repeaters_only;
};
MarkerCluster s_clusters[kMaxMapMarkers];
int s_marker_cluster[kMaxMapMarkers];
int s_cluster_count = 0;
bool s_clusters_valid = false;
static int rebuild_map(bool include_markers);

static void update_clusters() {
  if (s_clusters_valid) return;
  s_cluster_count = 0;
  for (int i = 0; i < s_marker_count; ++i) {
    const MapMarker& m = s_marker_slots[i];
    s_marker_cluster[i] = -1;
    if (s.filter == FILTER_COMPANIONS && m.type != ADV_TYPE_CHAT) continue;
    if (s.filter == FILTER_REPEATERS && m.type != ADV_TYPE_REPEATER) continue;
    int best = -1, distance = 48 * 48;
    for (int j = 0; j < s_cluster_count; ++j) {
      const int64_t dx = (int64_t)m.screen_x - s_clusters[j].x;
      const int64_t dy = (int64_t)m.screen_y - s_clusters[j].y;
      const int64_t d = dx * dx + dy * dy;
      if (d < distance) { best = j; distance = (int)d; }
    }
    if (best < 0) {
      best = s_cluster_count++;
      s_clusters[best] = {m.screen_x, m.screen_y, 0, i, 0, 0, true};
    }
    MarkerCluster& c = s_clusters[best];
    s_marker_cluster[i] = best;
    c.sum_x += m.screen_x;
    c.sum_y += m.screen_y;
    ++c.count;
    c.x = (int)(c.sum_x / c.count);
    c.y = (int)(c.sum_y / c.count);
    c.repeaters_only = c.repeaters_only && m.type == ADV_TYPE_REPEATER;
  }
  s_clusters_valid = true;
}

static inline double world_size_px(int zoom) {
  return 256.0 * (double)(1 << zoom);
}

static void latlon_to_world(double lat, double lon, int zoom, double* wx, double* wy) {
  const double n = world_size_px(zoom);
  *wx = (lon + 180.0) / 360.0 * n;
  lat = fmax(-85.05112878, fmin(85.05112878, lat));
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
  p.putBool("names", s.show_names);
  p.end();
}

static bool map_viewport_load() {
  Preferences p;
  if (!p.begin("map", true)) return false;
  const int32_t lat_e6 = p.getInt("lat_e6", INT32_MIN);
  const int32_t lon_e6 = p.getInt("lon_e6", INT32_MIN);
  uint8_t z = p.getUChar("zoom", 0xFF);
  uint8_t f = p.getUChar("filt", 0xFF);
  s.show_names = p.getBool("names", false);
  p.end();

  if (lat_e6 == INT32_MIN || lon_e6 == INT32_MIN || z == 0xFF) return false;
  if (z < kMinZoom) z = kMinZoom;
  if (z > kMaxZoom) z = kMaxZoom;
  if (f > FILTER_REPEATERS) f = FILTER_ALL;

  if (lat_e6 < -85051129 || lat_e6 > 85051129 || lon_e6 < -180000000 || lon_e6 > 180000000) return false;
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

static void refresh_marker_snapshot() {
  s_clusters_valid = false;
  if (!g_mesh) return;
#if defined(ESP32)
  if (g_mesh_mutex && xSemaphoreTake(g_mesh_mutex, pdMS_TO_TICKS(10)) != pdTRUE) return;
#endif
  int count = 0;
  ContactsIterator it = g_mesh->startContactsIterator();
  ContactInfo ci;
  while (it.hasNext(g_mesh, ci) && count < kMaxMapMarkers) {
    if (ci.type != ADV_TYPE_CHAT && ci.type != ADV_TYPE_REPEATER) continue;
    if (!ci.gps_lat && !ci.gps_lon) continue;
    if (ci.gps_lat < -90000000 || ci.gps_lat > 90000000 ||
        ci.gps_lon < -180000000 || ci.gps_lon > 180000000) continue;
    MapMarker& marker = s_marker_slots[count++];
    if (marker.lat != ci.gps_lat || marker.lon != ci.gps_lon) {
      // Zoom-zero world coordinates are reusable at every zoom level.
      latlon_to_world((double)ci.gps_lat / 1.0e6, (double)ci.gps_lon / 1.0e6,
                      0, &marker.world_x, &marker.world_y);
      s_projection_valid = false;
    }
    marker.lat = ci.gps_lat;
    marker.lon = ci.gps_lon;
    marker.type = ci.type;
    char name[sizeof(marker.name)];
    memcpy(name, ci.name, sizeof(name));
    name[sizeof(name) - 1] = 0;
    if (strcmp(marker.name, name) != 0) {
      memcpy(marker.name, name, sizeof(name));
      // Measure only when the name changes, not on each map redraw.
      lv_txt_get_size(&marker.name_size, marker.name, &lv_font_montserrat_14,
                      0, 0, 180, LV_TEXT_FLAG_NONE);
    }
  }
#if defined(ESP32)
  if (g_mesh_mutex) xSemaphoreGive(g_mesh_mutex);
#endif
  // A failed lock leaves the previous snapshot intact.
  if (s_marker_count != count) s_projection_valid = false;
  s_marker_count = count;
}

static void update_marker_projection() {
  const int width = lv_obj_get_width(s.content);
  const int height = lv_obj_get_height(s.content);
  if (s_projection_valid && s_projected_lat == s.center_lat && s_projected_lon == s.center_lon &&
      s_projected_zoom == s.zoom && s_projected_width == width && s_projected_height == height &&
      s_projected_self_valid == g_fixed_position_valid &&
      s_projected_self_lat == g_fixed_position_lat && s_projected_self_lon == g_fixed_position_lon) return;
  double cx, cy;
  latlon_to_world(s.center_lat, s.center_lon, s.zoom, &cx, &cy);
  const double scale = (double)(1 << s.zoom);
  if (s_neighbour_origin_valid) {
    s_neighbour_origin.screen_x = (int)round(s_neighbour_origin.world_x * scale - cx) + width / 2;
    s_neighbour_origin.screen_y = (int)round(s_neighbour_origin.world_y * scale - cy) + height / 2;
  }
  for (int i = 0; i < s_neighbour_count; ++i) {
    MapMarker& point = s_neighbour_points[i];
    point.screen_x = (int)round(point.world_x * scale - cx) + width / 2;
    point.screen_y = (int)round(point.world_y * scale - cy) + height / 2;
  }
  s_clusters_valid = false;
  for (int i = 0; i < s_marker_count; ++i) {
    MapMarker& marker = s_marker_slots[i];
    marker.screen_x = (int)round(marker.world_x * scale - cx) + width / 2;
    marker.screen_y = (int)round(marker.world_y * scale - cy) + height / 2;
  }
  if (g_fixed_position_valid) {
    double wx, wy;
    latlon_to_world(g_fixed_position_lat, g_fixed_position_lon, s.zoom, &wx, &wy);
    s_self_x = (int)round(wx - cx) + width / 2;
    s_self_y = (int)round(wy - cy) + height / 2;
  }
  s_projected_lat = s.center_lat;
  s_projected_lon = s.center_lon;
  s_projected_zoom = s.zoom;
  s_projected_width = width;
  s_projected_height = height;
  s_projected_self_lat = g_fixed_position_lat;
  s_projected_self_lon = g_fixed_position_lon;
  s_projected_self_valid = g_fixed_position_valid;
  s_projection_valid = true;
}

static bool marker_position(const MapMarker& marker, int* x, int* y) {
  if (s.filter == FILTER_COMPANIONS && marker.type != ADV_TYPE_CHAT) return false;
  if (s.filter == FILTER_REPEATERS && marker.type != ADV_TYPE_REPEATER) return false;
  *x = marker.screen_x + s.drag_dx;
  *y = marker.screen_y + s.drag_dy;
  return *x >= -12 && *y >= -12 && *x <= s_projected_width + 12 &&
         *y <= s_projected_height + 12;
}

static void draw_marker(lv_draw_ctx_t* ctx, int x, int y, uint32_t color, int radius) {
  if (x + radius < ctx->clip_area->x1 || x - radius > ctx->clip_area->x2 ||
      y + radius < ctx->clip_area->y1 || y - radius > ctx->clip_area->y2) return;
  lv_draw_rect_dsc_t dot;
  lv_draw_rect_dsc_init(&dot);
  dot.bg_color = lv_color_hex(color);
  dot.bg_opa = LV_OPA_COVER;
  dot.radius = LV_RADIUS_CIRCLE;
  dot.border_width = 1;
  dot.border_color = lv_color_white();
  lv_area_t area = {(lv_coord_t)(x - radius), (lv_coord_t)(y - radius),
                    (lv_coord_t)(x + radius), (lv_coord_t)(y + radius)};
  lv_draw_rect(ctx, &dot, &area);
}

static void markers_draw_cb(lv_event_t* e) {
  update_marker_projection();
  update_clusters();
  lv_draw_ctx_t* ctx = lv_event_get_draw_ctx(e);
  lv_area_t origin;
  lv_obj_get_coords(s_marker_layer, &origin);
  s_neighbour_draw = {};
  s_neighbour_draw.seen = true;
  if (s_neighbour_active && s_neighbour_origin_valid) {
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = lv_color_black();
    line.width = 2;
    line.opa = LV_OPA_COVER;
    for (int i = 0; i < s_neighbour_count; ++i) {
      ++s_neighbour_draw.attempted;
      double x = s_neighbour_origin.screen_x + s.drag_dx;
      double y = s_neighbour_origin.screen_y + s.drag_dy;
      double ex = s_neighbour_points[i].screen_x + s.drag_dx;
      double ey = s_neighbour_points[i].screen_y + s.drag_dy;
      if (!NeighbourMap::clip(x, y, ex, ey, s_projected_width, s_projected_height)) {
        ++s_neighbour_draw.clipped;
        continue;
      }
      lv_point_t a = {(lv_coord_t)(origin.x1 + lround(x)), (lv_coord_t)(origin.y1 + lround(y))};
      lv_point_t b = {(lv_coord_t)(origin.x1 + lround(ex)), (lv_coord_t)(origin.y1 + lround(ey))};
      lv_draw_line(ctx, &line, &a, &b);
      ++s_neighbour_draw.drawn;
      if (a.x == b.x && a.y == b.y) ++s_neighbour_draw.coincident;
      if (!s_dragging) {
        char text[24];
        snprintf(text, sizeof(text), "%+.2f dB", s_neighbour_points[i].snr_x4 / 4.0);
        lv_draw_label_dsc_t label;
        lv_draw_label_dsc_init(&label);
        label.font = &lv_font_montserrat_12;
        label.color = lv_color_white();
        label.align = LV_TEXT_ALIGN_CENTER;
        const int mx = LV_CLAMP(origin.x1 + 40, (a.x + b.x) / 2, origin.x2 - 40);
        const int my = LV_CLAMP(origin.y1 + 12, (a.y + b.y) / 2, origin.y2 - 12);
        lv_area_t box = {(lv_coord_t)(mx - 38), (lv_coord_t)(my - 9),
                         (lv_coord_t)(mx + 38), (lv_coord_t)(my + 9)};
        lv_draw_rect_dsc_t background;
        lv_draw_rect_dsc_init(&background);
        background.bg_color = lv_color_hex(0x287DB4);
        background.bg_opa = LV_OPA_80;
        background.radius = 3;
        lv_draw_rect(ctx, &background, &box);
        box.y1 += 2;
        lv_draw_label(ctx, &label, &box, text, nullptr);
      }
    }
  }
  // Draw labels before dots so labels cannot cover another node.
  if (s.show_names && !(s_dragging && s_gesture_travel > 10)) {
    int names = 0;
    for (int i = 0; i < s_cluster_count && names < 22; ++i) {
      if (s_clusters[i].count != 1) continue;
      int x, y;
      const MapMarker& marker = s_marker_slots[s_clusters[i].first];
      if (!marker.name[0] || !marker_position(marker, &x, &y)) continue;
      const int width = marker.name_size.x + 10;
      const int height = marker.name_size.y + 6;
      int left = origin.x1 + x + 8;
      if (left + width > origin.x2 - 4) left = origin.x1 + x - width - 8;
      left = LV_CLAMP(origin.x1 + 4, left, origin.x2 - width - 3);
      const int top = LV_CLAMP(origin.y1 + 4, origin.y1 + y - height / 2,
                               origin.y2 - height - 3);
      lv_area_t background = {(lv_coord_t)left, (lv_coord_t)top,
                              (lv_coord_t)(left + width - 1), (lv_coord_t)(top + height - 1)};
      if (background.x2 < ctx->clip_area->x1 || background.x1 > ctx->clip_area->x2 ||
          background.y2 < ctx->clip_area->y1 || background.y1 > ctx->clip_area->y2) {
        ++names;
        continue;
      }
      lv_draw_rect_dsc_t backdrop;
      lv_draw_rect_dsc_init(&backdrop);
      backdrop.bg_color = lv_color_hex(0x62686E);
      backdrop.bg_opa = LV_OPA_70;
      backdrop.border_color = lv_color_hex(0x9AA0A6);
      backdrop.border_opa = LV_OPA_30;
      backdrop.border_width = 1;
      backdrop.radius = 3;
      lv_draw_rect(ctx, &backdrop, &background);
      lv_draw_label_dsc_t label;
      lv_draw_label_dsc_init(&label);
      label.font = &lv_font_montserrat_14;
      label.color = lv_color_white();
      lv_area_t area = {(lv_coord_t)(left + 5), (lv_coord_t)(top + 3),
                        (lv_coord_t)(left + width - 6), (lv_coord_t)(top + height - 4)};
      lv_draw_label(ctx, &label, &area, marker.name, nullptr);
      ++names;
    }
  }
  for (int i = 0; i < s_cluster_count; ++i) {
    const MarkerCluster& c = s_clusters[i];
    const int x = origin.x1 + c.x + s.drag_dx;
    const int y = origin.y1 + c.y + s.drag_dy;
    const int radius = c.count > 1 ? 17 : 4;
    if (x + radius < ctx->clip_area->x1 || x - radius > ctx->clip_area->x2 ||
        y + radius < ctx->clip_area->y1 || y - radius > ctx->clip_area->y2) continue;
    draw_marker(ctx, x, y, c.repeaters_only ? 0xFF3B30 : 0x35A4FF, radius);
    if (c.count > 1) {
      char count[12];
      snprintf(count, sizeof(count), "%d", c.count);
      lv_draw_label_dsc_t label;
      lv_draw_label_dsc_init(&label);
      label.font = &lv_font_montserrat_14;
      label.color = lv_color_white();
      label.align = LV_TEXT_ALIGN_CENTER;
      lv_area_t area = {(lv_coord_t)(x - 17), (lv_coord_t)(y - 7),
                        (lv_coord_t)(x + 17), (lv_coord_t)(y + 10)};
      lv_draw_label(ctx, &label, &area, count, nullptr);
    }
  }
  if (s_neighbour_active) {
    if (s_neighbour_origin_valid) {
      const int x = s_neighbour_origin.screen_x + s.drag_dx;
      const int y = s_neighbour_origin.screen_y + s.drag_dy;
      if (x >= 0 && y >= 0 && x < s_projected_width && y < s_projected_height)
        draw_marker(ctx, origin.x1 + x, origin.y1 + y, 0x28C8BE, 9);
    }
    for (int i = 0; i < s_neighbour_count; ++i) {
      const int x = s_neighbour_points[i].screen_x + s.drag_dx;
      const int y = s_neighbour_points[i].screen_y + s.drag_dy;
      if (x < 0 || y < 0 || x >= s_projected_width || y >= s_projected_height) continue;
      draw_marker(ctx, origin.x1 + x, origin.y1 + y, 0xE5A93D, 6);
      if (s.show_names && !s_dragging && s_neighbour_points[i].name[0]) {
        char text[19];
        snprintf(text, sizeof(text), "%.18s", s_neighbour_points[i].name);
        const int left = LV_CLAMP(origin.x1 + 4, origin.x1 + x - 70, origin.x2 - 144);
        const int top = LV_CLAMP(origin.y1 + 4, origin.y1 + y + 9, origin.y2 - 24);
        lv_area_t box = {(lv_coord_t)left, (lv_coord_t)top, (lv_coord_t)(left + 140), (lv_coord_t)(top + 20)};
        lv_draw_rect_dsc_t background;
        lv_draw_rect_dsc_init(&background);
        background.bg_color = lv_color_hex(0x62686E);
        background.bg_opa = LV_OPA_70;
        background.radius = 3;
        lv_draw_rect(ctx, &background, &box);
        lv_draw_label_dsc_t label;
        lv_draw_label_dsc_init(&label);
        label.font = &lv_font_montserrat_12;
        label.color = lv_color_white();
        label.align = LV_TEXT_ALIGN_CENTER;
        box.y1 += 2;
        lv_draw_label(ctx, &label, &box, text, nullptr);
      }
    }
  }
  if (g_fixed_position_valid) {
    draw_marker(ctx, origin.x1 + s_self_x + s.drag_dx,
                origin.y1 + s_self_y + s.drag_dy, 0x6DC264, 5);
  }
}

static void content_pressed_cb(lv_event_t*) {
  s_gesture_travel = 0;
  s_dragging = true;
}

static void content_clicked_cb(lv_event_t*) {
  if (s_gesture_travel > 10 || !lv_indev_get_act()) return;
  update_marker_projection();
  lv_point_t point;
  lv_indev_get_point(lv_indev_get_act(), &point);
  lv_area_t area;
  lv_obj_get_coords(s.content, &area);
  point.x -= area.x1;
  point.y -= area.y1;
  update_clusters();
  int nearest = -1, nearest_distance = 24 * 24;
  for (int i = 0; i < s_cluster_count; ++i) {
    const MarkerCluster& c = s_clusters[i];
    if (c.count < 2) continue;
    const int64_t dx = (int64_t)c.x + s.drag_dx - point.x;
    const int64_t dy = (int64_t)c.y + s.drag_dy - point.y;
    const int64_t d = dx * dx + dy * dy;
    if (d < nearest_distance) { nearest = i; nearest_distance = (int)d; }
  }
  if (nearest >= 0 && s.zoom < kMaxZoom) {
    double wx, wy;
    latlon_to_world(s.center_lat, s.center_lon, s.zoom, &wx, &wy);
    wx += s_clusters[nearest].x - s_projected_width / 2;
    wy += s_clusters[nearest].y - s_projected_height / 2;
    world_to_latlon(wx, wy, s.zoom, &s.center_lat, &s.center_lon);
    ++s.zoom;
    s.drag_dx = s.drag_dy = s_pending_dx = s_pending_dy = 0;
    (void)rebuild_map(true);
    map_viewport_mark_dirty();
    return;
  }
  if (nearest >= 0) {
    point.x = s_clusters[nearest].x + s.drag_dx;
    point.y = s_clusters[nearest].y + s.drag_dy;
  }
  char details[1024] = {};
  size_t used = 0;
  for (int i = 0; i < s_marker_count; ++i) {
    int x, y;
    const MapMarker& marker = s_marker_slots[i];
    if (nearest >= 0 && s_marker_cluster[i] != nearest) continue;
    if (!marker_position(marker, &x, &y)) continue;
    const int dx = x - point.x, dy = y - point.y;
    if (nearest < 0 && (int64_t)dx * dx + (int64_t)dy * dy > 20 * 20) continue;
    const int n = snprintf(details + used, sizeof(details) - used, "%s%s (%s)\n%.6f, %.6f\n",
                           used ? "\n" : "", marker.name[0] ? marker.name : "Unnamed",
                           marker.type == ADV_TYPE_REPEATER ? "Repeater" : "Companion",
                           (double)marker.lat / 1.0e6, (double)marker.lon / 1.0e6);
    if (n < 0 || (size_t)n >= sizeof(details) - used) break;
    used += n;
  }
  if (!used) return;
  lv_obj_t* box = lv_msgbox_create(nullptr, "Nearby nodes", details, nullptr, true);
  if (!box) return;
  lv_obj_set_width(box, LV_MIN(SCR_W - 32, 440));
  lv_obj_set_style_max_height(box, SCR_H - 80, 0);
  lv_obj_t* body = lv_msgbox_get_content(box);
  lv_obj_set_style_max_height(body, SCR_H - 160, 0);
  lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_center(box);
}

static void ensure_marker_slots() {
  if (!s.content || s_marker_slots_ready) return;
  s_marker_layer = lv_obj_create(s.content);
  lv_obj_remove_style_all(s_marker_layer);
  lv_obj_set_size(s_marker_layer, lv_pct(100), lv_pct(100));
  lv_obj_set_pos(s_marker_layer, 0, 0);
  lv_obj_clear_flag(s_marker_layer, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_move_to_index(s_marker_layer, 1);
  lv_obj_add_event_cb(s_marker_layer, markers_draw_cb, LV_EVENT_DRAW_MAIN, nullptr);
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
  if (!sd_is_mounted()) {
    for (int i = 0; i < kMaxTileSlots; i++) {
      if (s_tile_slots[i].img) lv_obj_add_flag(s_tile_slots[i].img, LV_OBJ_FLAG_HIDDEN);
      if (s_tile_slots[i].ph)  lv_obj_add_flag(s_tile_slots[i].ph, LV_OBJ_FLAG_HIDDEN);
    }
    // Node markers remain usable without SD background tiles.
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

  if (include_markers) refresh_marker_snapshot();
  update_marker_projection();
  int drawn_pins = 0;
  int hidden_pins = 0;
  for (int i = 0; i < s_marker_count; ++i) {
    int x, y;
    if (marker_position(s_marker_slots[i], &x, &y)) ++drawn_pins;
  }
  if (s_marker_layer) lv_obj_invalidate(s_marker_layer);

  update_overlay_labels(loaded_tiles, total_tiles, drawn_pins, hidden_pins);
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

static void fit_neighbour_view() {
  if (!s_neighbour_count && !s_neighbour_origin_valid) return;
  const MapMarker& first = s_neighbour_origin_valid ? s_neighbour_origin : s_neighbour_points[0];
  double min_x = first.world_x, min_y = first.world_y, max_x = min_x, max_y = min_y;
  for (int i = 0; i < s_neighbour_count; ++i) {
    min_x = fmin(min_x, s_neighbour_points[i].world_x);
    max_x = fmax(max_x, s_neighbour_points[i].world_x);
    min_y = fmin(min_y, s_neighbour_points[i].world_y);
    max_y = fmax(max_y, s_neighbour_points[i].world_y);
  }
  world_to_latlon((min_x + max_x) / 2, (min_y + max_y) / 2, 0, &s.center_lat, &s.center_lon);
  s.zoom = kMaxZoom;
  while (s.zoom > kMinZoom &&
      ((max_x - min_x) * (1 << s.zoom) > lv_obj_get_width(s.content) - 120 ||
       (max_y - min_y) * (1 << s.zoom) > lv_obj_get_height(s.content) - 180)) --s.zoom;
  s_projection_valid = false;
  (void)rebuild_map(true);
}

static void process_repeater_position() {
  if (!s_neighbour_active || !g_mesh) return;
  RepeaterPosition position;
  uint8_t source[32];
  portENTER_CRITICAL(&s_neighbour_mux);
  position = s_remote_position;
  memcpy(source, s_neighbour_reply.source, sizeof(source));
  portEXIT_CRITICAL(&s_neighbour_mux);
  if (position.stage == PositionStage::NeedLat || position.stage == PositionStage::NeedLon) {
    if (g_mesh_mutex && xSemaphoreTake(g_mesh_mutex, pdMS_TO_TICKS(10)) != pdTRUE) return;
    ContactInfo* contact = g_mesh->lookupContactByPubKey(source, 32);
    const bool latitude = position.stage == PositionStage::NeedLat;
    const PositionStage waiting = latitude ? PositionStage::WaitLat : PositionStage::WaitLon;
    char command[20];
    snprintf(command, sizeof(command), "%02X|get %s", (uint8_t)(position.nonce + (latitude ? 0 : 1)),
             latitude ? "lat" : "lon");
    portENTER_CRITICAL(&s_neighbour_mux);
    s_remote_position.stage = waiting;
    s_remote_position.started = millis();
    portEXIT_CRITICAL(&s_neighbour_mux);
    uint32_t timeout = 60000;
    const int result = contact ? g_mesh->sendCommandData(*contact,
      g_mesh->getRTCClock()->getCurrentTimeUnique(), 0, command, timeout) : MSG_SEND_FAILED;
    if (g_mesh_mutex) xSemaphoreGive(g_mesh_mutex);
    Serial.printf("[neighbour-diag] origin query: key=%02X%02X%02X%02X command=\"%s\" queued=%s timeout=%lu ms\n",
                  source[0], source[1], source[2], source[3], command,
                  result == MSG_SEND_FAILED ? "no" : "yes", (unsigned long)timeout);
    portENTER_CRITICAL(&s_neighbour_mux);
    s_remote_position.timeout = LV_CLAMP(60000U, timeout, 120000U);
    if (result == MSG_SEND_FAILED) s_remote_position.stage = PositionStage::Failed;
    portEXIT_CRITICAL(&s_neighbour_mux);
  } else if ((position.stage == PositionStage::WaitLat || position.stage == PositionStage::WaitLon) &&
             (uint32_t)(millis() - position.started) > position.timeout) {
    portENTER_CRITICAL(&s_neighbour_mux);
    if (s_remote_position.stage == position.stage) s_remote_position.stage = PositionStage::Failed;
    portEXIT_CRITICAL(&s_neighbour_mux);
    Serial.printf("[neighbour-diag] origin query timed out: %s after %lu ms; no line origin\n",
                  position.stage == PositionStage::WaitLat ? "latitude" : "longitude",
                  (unsigned long)(millis() - position.started));
  } else if (position.stage == PositionStage::Ready && !s_neighbour_origin_valid) {
    s_neighbour_origin_valid = true;
    latlon_to_world(position.lat, position.lon, 0, &s_neighbour_origin.world_x, &s_neighbour_origin.world_y);
    fit_neighbour_view();
    request_draw_diagnostics();
    Serial.printf("[neighbour-diag] origin READY: repeater=\"%.31s\" GPS=%.7f,%.7f; connecting %d mapped neighbours\n",
                  s_neighbour_name, position.lat, position.lon, s_neighbour_count);
  }
}

static void marker_refresh_timer_cb(lv_timer_t*) {
  if (!ui_mapscreen || lv_scr_act() != ui_mapscreen || s_dragging) return;
  process_repeater_position();
  NeighbourReply reply;
  portENTER_CRITICAL(&s_neighbour_mux);
  reply = s_neighbour_reply;
  portEXIT_CRITICAL(&s_neighbour_mux);
  const bool fresh_reply = reply.ready;
  if (fresh_reply) {
    s_neighbour_snapshot = reply;
    s_neighbour_snapshot_valid = true;
    // Allocate only for a manual neighbour response, never from internal heap.
    if (!s_diagnostics) {
      s_diagnostics = static_cast<NeighbourDiagnostics*>(heap_caps_calloc(
          1, sizeof(NeighbourDiagnostics), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
      if (!s_diagnostics) Serial.println("[neighbour-diag] PSRAM unavailable; inventory dump skipped");
    }
  } else if (s_neighbour_active && s_neighbour_snapshot_valid) {
    // An advert received after the neighbour reply may supply missing GPS data.
    reply = s_neighbour_snapshot;
  }
  if (reply.ready && g_mesh) {
    if (g_mesh_mutex && xSemaphoreTake(g_mesh_mutex, pdMS_TO_TICKS(10)) != pdTRUE) return;
    const int previous_count = s_neighbour_count;
    bool positions_changed = false;
    s_neighbour_count = 0;
    s_advert_mapped = 0;
    s_neighbour_unknown = s_neighbour_no_gps = s_neighbour_ambiguous = s_neighbour_other = 0;
    const int reported = reply.bytes[6] | (reply.bytes[7] << 8);
    s_neighbour_reported = reported;
    const int count = NeighbourMap::entryCount(reply.len, reported);
    if (fresh_reply) snapshot_detected_repeaters(reply);
    for (int i = 0; i < count; ++i) {
      ContactInfo ci{}, found{};
      int matches = 0;
      auto it = g_mesh->startContactsIterator();
      while (it.hasNext(g_mesh, ci)) {
        if (memcmp(ci.id.pub_key, reply.bytes + 8 + i * 9, 4) == 0) { found = ci; ++matches; }
      }
      const unsigned saved_matches = matches;
      unsigned advert_matches = 0;
      if (fresh_reply && s_diagnostics) {
        portENTER_CRITICAL(&s_advert_location_mux);
        if (s_advert_locations) advert_matches = s_advert_locations->matchCount(reply.bytes + 8 + i * 9, 4);
        portEXIT_CRITICAL(&s_advert_location_mux);
      }
      bool from_advert = false;
      AdvertLocations::Location record;
      bool cached = false;
      if (matches == 0 || (matches == 1 && found.type == ADV_TYPE_REPEATER &&
          !NeighbourMap::validPosition(found.gps_lat / 1.e6, found.gps_lon / 1.e6))) {
        portENTER_CRITICAL(&s_advert_location_mux);
        if (s_advert_locations) cached = s_advert_locations->findUnique(
          matches == 1 ? found.id.pub_key : reply.bytes + 8 + i * 9, matches == 1 ? 32 : 4, record);
        portEXIT_CRITICAL(&s_advert_location_mux);
      }
      if (cached) {
        if (!matches) {
          found = ContactInfo{};
          memcpy(found.name, record.name, sizeof(found.name));
          found.name[31] = 0;
          found.type = ADV_TYPE_REPEATER;
        }
        found.gps_lat = record.lat;
        found.gps_lon = record.lon;
        matches = 1;
        from_advert = true;
      }
      const char* reason = "mapped";
      if (!matches) { ++s_neighbour_unknown; reason = "no matching saved contact or received LoRa location advert"; }
      else if (matches > 1) { ++s_neighbour_ambiguous; reason = "ambiguous key prefix"; }
      else if (found.type != ADV_TYPE_REPEATER) { ++s_neighbour_other; reason = "not a repeater"; }
      else if (!NeighbourMap::validPosition(found.gps_lat / 1.e6, found.gps_lon / 1.e6)) {
        ++s_neighbour_no_gps; reason = "contact has no valid GPS position";
      }
      if (fresh_reply && s_diagnostics && s_diagnostics->match_count < 20) {
        auto& row = s_diagnostics->matches[s_diagnostics->match_count++];
        memcpy(row.key, reply.bytes + 8 + i * 9, sizeof(row.key));
        memcpy(&row.heard_seconds_ago, reply.bytes + 8 + i * 9 + 4, sizeof(row.heard_seconds_ago));
        row.snr_x4 = (int8_t)reply.bytes[8 + i * 9 + 8];
        // The dropdown can include sticky entries without a current GPS contact.
        // Caller owns LVGL, so compare its actual entries without refreshing it.
        for (int j = 0; j < g_repeater_count && j < MAX_DD_ENTRIES; ++j) {
          if (memcmp(g_repeater_list[j].pub_key, row.key, sizeof(row.key)) != 0) continue;
          ++row.screen_matches;
          snprintf(row.screen_name, sizeof(row.screen_name), "%.31s", g_repeater_list[j].name);
        }
        snprintf(row.name, sizeof(row.name), "%s", matches == 1 ? found.name : "(unresolved)");
        row.saved_matches = saved_matches; row.advert_matches = advert_matches;
        row.lat = found.gps_lat; row.lon = found.gps_lon;
        row.reason = reason; row.from_advert = from_advert;
      }
      if (strcmp(reason, "mapped") != 0) continue;
      if (from_advert) ++s_advert_mapped;
      MapMarker& point = s_neighbour_points[s_neighbour_count++];
      double wx, wy;
      latlon_to_world(found.gps_lat / 1.e6, found.gps_lon / 1.e6, 0, &wx, &wy);
      if (point.world_x != wx || point.world_y != wy) positions_changed = true;
      point.world_x = wx;
      point.world_y = wy;
      memcpy(point.name, found.name, sizeof(point.name));
      point.name[31] = 0;
      point.snr_x4 = (int8_t)reply.bytes[8 + i * 9 + 8];
    }
    if (g_mesh_mutex) xSemaphoreGive(g_mesh_mutex);
    if (fresh_reply && s_diagnostics) s_diagnostics->ready = true;
    portENTER_CRITICAL(&s_neighbour_mux);
    s_neighbour_reply.ready = false;
    portEXIT_CRITICAL(&s_neighbour_mux);
    if (fresh_reply || previous_count != s_neighbour_count || positions_changed) {
      s_projection_valid = false;
      fit_neighbour_view();
      request_draw_diagnostics();
      Serial.printf("[neighbour-map] %d/%d mapped; unknown=%d noGPS=%d ambiguous=%d other=%d\n",
        s_neighbour_count, reported, s_neighbour_unknown, s_neighbour_no_gps,
        s_neighbour_ambiguous, s_neighbour_other);
    }
  } else if (reply.waiting && (uint32_t)(millis() - reply.started) > reply.timeout) {
    portENTER_CRITICAL(&s_neighbour_mux);
    s_neighbour_reply.waiting = false;
    portEXIT_CRITICAL(&s_neighbour_mux);
    s_neighbour_reported = -2;
    Serial.printf("[neighbour-diag] neighbour reply TIMEOUT: key=%02X%02X%02X%02X tag=%08lX waited=%lu ms\n",
                  reply.source[0], reply.source[1], reply.source[2], reply.source[3],
                  (unsigned long)reply.tag, (unsigned long)(millis() - reply.started));
  }
  if (s_neighbour_active) {
    portENTER_CRITICAL(&s_neighbour_mux);
    const PositionStage stage = s_remote_position.stage;
    portEXIT_CRITICAL(&s_neighbour_mux);
    const char* status = s_neighbour_origin_valid ? "Live repeater position" :
      stage == PositionStage::Failed ? "Position request failed/timed out (admin login required)" :
      stage == PositionStage::Unset ? "Repeater has no configured position" : "Requesting repeater position...";
    if (s_neighbour_reported >= 0)
      lv_label_set_text_fmt(s_neighbour_label,
        "%s: %d/%d mapped (%d LoRa adverts)\n%s\nUnknown: %d  No GPS: %d  Ambiguous: %d  Other: %d%s", s_neighbour_name,
        s_neighbour_count, s_neighbour_reported, s_advert_mapped, status, s_neighbour_unknown, s_neighbour_no_gps,
        s_neighbour_ambiguous, s_neighbour_other,
        s_neighbour_unknown ? "\nWaiting for neighbour location adverts" : "");
    else lv_label_set_text_fmt(s_neighbour_label, "%s: %s\n%s", s_neighbour_name,
      s_neighbour_reported == -2 ? "No neighbour reply" : "Waiting for neighbours...", status);
  }
  refresh_marker_snapshot();
  if (s_marker_layer) lv_obj_invalidate(s_marker_layer);
}

static void map_loaded_cb(lv_event_t*) {
  refresh_marker_snapshot();
  if (s_marker_layer) lv_obj_invalidate(s_marker_layer);
}

static void content_pressing_cb(lv_event_t*) {
  lv_indev_t* indev = lv_indev_get_act();
  if (!indev) return;
  lv_point_t v;
  lv_indev_get_vect(indev, &v);
  if (v.x == 0 && v.y == 0) return;

  s_gesture_travel += abs(v.x) + abs(v.y);

  s.drag_dx += v.x;
  s.drag_dy += v.y;
  if (s_marker_layer) lv_obj_invalidate(s_marker_layer);
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
  s_dragging = false;
  refresh_marker_snapshot();
  if (s_marker_layer) lv_obj_invalidate(s_marker_layer);
  if (s.drag_dx == 0 && s.drag_dy == 0) {
    s_pending_dx = 0;
    s_pending_dy = 0;
    return;
  }

  apply_pending_drag_translation();

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

  // Tiles reuse their prefetch ring; markers always use the latest center.
  if (should_rebuild_after_drag()) (void)rebuild_map(true);
  if (s_marker_layer) lv_obj_invalidate(s_marker_layer);
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

static void cb_names_toggle(lv_event_t* e) {
  s.show_names = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
  if (s_marker_layer) lv_obj_invalidate(s_marker_layer);
  map_viewport_mark_dirty();
}

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

void map_record_repeater_advert(const uint8_t* key, const char* name, int32_t lat, int32_t lon, uint32_t timestamp) {
  if (!key || !name || !NeighbourMap::validPosition(lat / 1.e6, lon / 1.e6)) return;
  portENTER_CRITICAL(&s_advert_location_mux);
  const bool initialized = s_advert_locations != nullptr;
  portEXIT_CRITICAL(&s_advert_location_mux);
  if (!initialized) {
    void* memory = heap_caps_malloc(sizeof(AdvertCache), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!memory) return; // Do not consume scarce internal radio/TLS memory.
    AdvertCache* cache = new (memory) AdvertCache{};
    portENTER_CRITICAL(&s_advert_location_mux);
    const bool installed = s_advert_locations == nullptr;
    if (installed) s_advert_locations = cache;
    portEXIT_CRITICAL(&s_advert_location_mux);
    if (!installed) { cache->~AdvertCache(); heap_caps_free(memory); }
  }
  portENTER_CRITICAL(&s_advert_location_mux);
  s_advert_locations->record(key, name, lat, lon, timestamp);
  portEXIT_CRITICAL(&s_advert_location_mux);
}

void map_clear_advert_locations(void) {
  portENTER_CRITICAL(&s_advert_location_mux);
  if (s_advert_locations) s_advert_locations->clear();
  portEXIT_CRITICAL(&s_advert_location_mux);
}

bool map_get_repeater_advert(const uint8_t* key, char* name, size_t name_size,
                             int32_t* lat, int32_t* lon, uint32_t* timestamp) {
  AdvertLocations::Location location;
  portENTER_CRITICAL(&s_advert_location_mux);
  const bool found = s_advert_locations && s_advert_locations->findUnique(key, 32, location);
  portEXIT_CRITICAL(&s_advert_location_mux);
  if (!found) return false;
  if (name && name_size) snprintf(name, name_size, "%s", location.name);
  if (lat) *lat = location.lat;
  if (lon) *lon = location.lon;
  if (timestamp) *timestamp = location.timestamp;
  return true;
}


void map_receive_neighbours(const uint8_t* source, const uint8_t* data, size_t len) {
  if (!source || !data || len < 4) return;
  uint32_t tag;
  memcpy(&tag, data, sizeof(tag));
  portENTER_CRITICAL(&s_neighbour_mux);
  const bool waiting = s_neighbour_reply.waiting;
  const uint32_t expected_tag = s_neighbour_reply.tag;
  const bool matching_source = memcmp(source, s_neighbour_reply.source, 32) == 0;
  const bool accepted = waiting && tag == expected_tag && matching_source && len >= 8;
  if (accepted) {
    s_neighbour_reply.len = LV_MIN(len, sizeof(s_neighbour_reply.bytes));
    memcpy(s_neighbour_reply.bytes, data, s_neighbour_reply.len);
    s_neighbour_reply.waiting = false;
    s_neighbour_reply.ready = true;
    // Wait for the neighbour reply before CLI requests to avoid replay ordering issues.
    if (s_remote_position.stage == PositionStage::Idle) s_remote_position.stage = PositionStage::NeedLat;
  }
  portEXIT_CRITICAL(&s_neighbour_mux);
  if (waiting && (matching_source || tag == expected_tag)) {
    const unsigned total = len >= 6 ? data[4] | (data[5] << 8) : 0;
    const unsigned reported = len >= 8 ? data[6] | (data[7] << 8) : 0;
    const char* result = accepted ? "ACCEPTED" : !matching_source ? "IGNORED-source" :
                         tag != expected_tag ? "IGNORED-tag" : "IGNORED-short";
    Serial.printf("[neighbour-diag] reply %s: source=%02X%02X%02X%02X tag=%08lX expected=%08lX bytes=%u total=%u reported=%u usable=%d\n",
                  result, source[0], source[1], source[2], source[3],
                  (unsigned long)tag, (unsigned long)expected_tag, (unsigned)len,
                  total, reported, NeighbourMap::entryCount(len, reported));
  }
}

void map_receive_repeater_position(const uint8_t* source, const char* text) {
  if (!text) return;
  RepeaterPosition position;
  bool matching_source;
  portENTER_CRITICAL(&s_neighbour_mux);
  position = s_remote_position;
  matching_source = memcmp(source, s_neighbour_reply.source, 32) == 0;
  portEXIT_CRITICAL(&s_neighbour_mux);
  const bool latitude = position.stage == PositionStage::WaitLat;
  if (!matching_source || (!latitude && position.stage != PositionStage::WaitLon)) return;
  char prefix[4];
  snprintf(prefix, sizeof(prefix), "%02X|", (uint8_t)(position.nonce + (latitude ? 0 : 1)));
  if (strncmp(text, prefix, 3) != 0) {
    Serial.printf("[neighbour-diag] origin reply ignored: key=%02X%02X%02X%02X expected_prefix=%.3s got_prefix=%.3s\n",
                  source[0], source[1], source[2], source[3], prefix, text);
    return;
  }
  const char* number = text + 3;
  while (*number == ' ') ++number;
  if (*number == '>') ++number;
  char* end;
  const double value = strtod(number, &end);
  const bool parsed = end != number;
  while (*end == ' ' || *end == '\r' || *end == '\n' || *end == '\t') ++end;
  const bool valid = parsed && !*end && isfinite(value) &&
    value >= (latitude ? -90 : -180) && value <= (latitude ? 90 : 180);
  bool applied = false;
  bool unset = false;
  portENTER_CRITICAL(&s_neighbour_mux);
  if (s_remote_position.stage == position.stage && s_remote_position.nonce == position.nonce &&
      memcmp(source, s_neighbour_reply.source, 32) == 0) {
    applied = true;
    if (!valid) s_remote_position.stage = PositionStage::Failed;
    else if (latitude) {
      s_remote_position.lat = value;
      s_remote_position.stage = PositionStage::NeedLon;
    } else {
      s_remote_position.lon = value;
      s_remote_position.stage = NeighbourMap::validPosition(s_remote_position.lat, value) ?
        PositionStage::Ready : PositionStage::Unset;
      unset = s_remote_position.stage == PositionStage::Unset;
    }
  }
  portEXIT_CRITICAL(&s_neighbour_mux);
  if (applied) {
    char printable[80];
    size_t i = 0;
    for (; i + 1 < sizeof(printable) && text[i]; ++i)
      printable[i] = (unsigned char)text[i] < 32 ? ' ' : text[i];
    printable[i] = 0;
    Serial.printf("[neighbour-diag] origin reply %s: %s value=%.7f raw=\"%s\"%s\n",
                  valid ? "VALID" : "INVALID", latitude ? "latitude" : "longitude", value,
                  printable, unset ? "; repeater position is unset (0,0)" : "");
  }
}

static void clear_neighbour_links(lv_event_t*) {
  clear_neighbour_diagnostics();
  portENTER_CRITICAL(&s_neighbour_mux);
  s_neighbour_reply.waiting = s_neighbour_reply.ready = false;
  s_remote_position.stage = PositionStage::Idle;
  portEXIT_CRITICAL(&s_neighbour_mux);
  s_neighbour_active = false;
  s_neighbour_snapshot_valid = false;
  s_neighbour_origin_valid = false;
  s_neighbour_count = 0;
  lv_label_set_text(s_neighbour_label, "");
  lv_obj_add_flag(s_neighbour_label, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(s_neighbour_clear, LV_OBJ_FLAG_HIDDEN);
  if (s_marker_layer) lv_obj_invalidate(s_marker_layer);
}

void map_show_neighbours(uint32_t tag, const uint8_t* source, const char* name, uint32_t timeout_ms) {
  clear_neighbour_diagnostics();
  s_neighbour_active = true;
  s_neighbour_count = 0;
  s_neighbour_origin_valid = false;
  s_neighbour_reported = -1;
  s_neighbour_snapshot_valid = false;
  snprintf(s_neighbour_name, sizeof(s_neighbour_name), "%s", name);
  portENTER_CRITICAL(&s_neighbour_mux);
  s_neighbour_reply = NeighbourReply{};
  s_neighbour_reply.tag = tag;
  memcpy(s_neighbour_reply.source, source, 32);
  s_neighbour_reply.started = millis();
  s_neighbour_reply.timeout = LV_CLAMP(60000U, timeout_ms, 120000U);
  s_neighbour_reply.waiting = true;
  s_remote_position = RepeaterPosition{};
  s_remote_position.stage = PositionStage::Idle;
  s_position_nonce += 2;
  s_remote_position.nonce = s_position_nonce;
  portEXIT_CRITICAL(&s_neighbour_mux);
  Serial.printf("[neighbour-diag] BEGIN repeater=\"%.31s\" key=%02X%02X%02X%02X%02X%02X%02X%02X tag=%08lX; waiting for list, then live origin\n",
                name, source[0], source[1], source[2], source[3], source[4], source[5], source[6], source[7],
                (unsigned long)tag);
  if (!s_diagnostic_timer) s_diagnostic_timer = lv_timer_create(neighbour_diagnostic_timer_cb, 100, nullptr);
  request_draw_diagnostics();
  // Arm reception before tile decoding: the radio may reply while the map loads.
  ui_mapscreen_screen_init();
  lv_label_set_text_fmt(s_neighbour_label, "%s: waiting for neighbours...", s_neighbour_name);
  lv_obj_clear_flag(s_neighbour_label, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(s_neighbour_clear, LV_OBJ_FLAG_HIDDEN);
  lv_scr_load(ui_mapscreen);
}

void map_viewport_save_if_dirty_flush() {
  map_viewport_save_now();
}

void ui_mapscreen_screen_init(void) {
  if (ui_mapscreen) return;

  s.center_lat = kInitLat;
  s.center_lon = kInitLon;
  s.zoom = kInitZoom;
  s.filter = FILTER_ALL;
  s.show_names = false;
  s.drag_dx = s.drag_dy = 0;
  s_pending_dx = s_pending_dy = 0;
  s_last_drag_apply_ms = millis();
  (void)map_viewport_load();

  lv_img_cache_set_size(kImgCacheEntries);
  if (s_marker_refresh_timer) lv_timer_resume(s_marker_refresh_timer);
  if (!s_marker_refresh_timer) {
    s_marker_refresh_timer = lv_timer_create(marker_refresh_timer_cb, 2000, nullptr);

  }

  ui_mapscreen = lv_obj_create(NULL);
  lv_obj_clear_flag(ui_mapscreen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(ui_mapscreen, map_loaded_cb, LV_EVENT_SCREEN_LOADED, nullptr);
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
  ui_battery_create(hdr);

  s.content = lv_obj_create(ui_mapscreen);
  lv_obj_set_size(s.content, SCR_W, SCR_H - STATUS_H - TAB_H);
  lv_obj_set_pos(s.content, 0, STATUS_H);
  lv_obj_set_style_bg_color(s.content, lv_color_hex(0x16202B), 0);
  lv_obj_set_style_bg_opa(s.content, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(s.content, 0, 0);
  lv_obj_set_style_radius(s.content, 0, 0);
  lv_obj_set_style_pad_all(s.content, 0, 0);
  lv_obj_clear_flag(s.content, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(s.content, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(s.content, content_pressed_cb, LV_EVENT_PRESSED, nullptr);
  lv_obj_add_event_cb(s.content, content_clicked_cb, LV_EVENT_CLICKED, nullptr);
  lv_obj_add_event_cb(s.content, content_released_cb, LV_EVENT_PRESS_LOST, nullptr);
  lv_obj_add_event_cb(s.content, content_pressing_cb, LV_EVENT_PRESSING, nullptr);
  lv_obj_add_event_cb(s.content, content_released_cb, LV_EVENT_RELEASED, nullptr);

  s.pan_layer = lv_obj_create(s.content);
  lv_obj_set_style_bg_opa(s.pan_layer, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(s.pan_layer, 0, 0);
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

  lv_obj_t* names_toggle = lv_checkbox_create(s.content);
  lv_checkbox_set_text(names_toggle, "Names");
  lv_obj_set_size(names_toggle, 120, 36);
  lv_obj_set_style_text_font(names_toggle, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(names_toggle, lv_color_white(), 0);
  lv_obj_set_style_bg_color(names_toggle, lv_color_hex(TH_SURFACE), 0);
  lv_obj_set_style_bg_opa(names_toggle, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(names_toggle, 6, 0);
  lv_obj_add_flag(names_toggle, LV_OBJ_FLAG_FLOATING);
  lv_obj_align(names_toggle, LV_ALIGN_TOP_RIGHT, -8, 46);
  if (s.show_names) lv_obj_add_state(names_toggle, LV_STATE_CHECKED);
  lv_obj_add_event_cb(names_toggle, cb_names_toggle, LV_EVENT_VALUE_CHANGED, nullptr);

  ui_tabbar_create(ui_mapscreen, 2);

  s_neighbour_label = lv_label_create(s.content);
  lv_obj_set_width(s_neighbour_label, SCR_W - 85);
  lv_obj_set_style_text_font(s_neighbour_label, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(s_neighbour_label, lv_color_white(), 0);
  lv_obj_set_style_bg_color(s_neighbour_label, lv_color_hex(0x62686E), 0);
  lv_obj_set_style_bg_opa(s_neighbour_label, LV_OPA_70, 0);
  lv_obj_set_style_pad_all(s_neighbour_label, 4, 0);
  lv_obj_align(s_neighbour_label, LV_ALIGN_BOTTOM_LEFT, 8, -8);
  lv_label_set_text(s_neighbour_label, "");
  s_neighbour_clear = make_btn(s.content, LV_SYMBOL_CLOSE, clear_neighbour_links, 32, 32, &lv_font_montserrat_14);
  lv_obj_align(s_neighbour_clear, LV_ALIGN_TOP_LEFT, 8, 68);
  lv_obj_add_flag(s_neighbour_label, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(s_neighbour_clear, LV_OBJ_FLAG_HIDDEN);

  (void)ensure_nonempty_map_view(true);
  if (!sd_is_mounted()) serialmon_append("maps: no SD mounted");
}

void ui_mapscreen_screen_destroy(void) {
  clear_neighbour_diagnostics();
  if (!ui_mapscreen) return;
  if (s_marker_refresh_timer) lv_timer_pause(s_marker_refresh_timer);
  lv_obj_del(ui_mapscreen);
  ui_mapscreen = nullptr;
  s.content = nullptr;
  s.pan_layer = nullptr;
  s.zoom_label = nullptr;
  s.mode_label = nullptr;
  s_neighbour_label = nullptr;
  s_neighbour_clear = nullptr;
  s_neighbour_active = false;
  s_neighbour_snapshot_valid = false;
  s_neighbour_origin_valid = false;
  s_neighbour_count = 0;
  portENTER_CRITICAL(&s_neighbour_mux);
  s_neighbour_reply.waiting = s_neighbour_reply.ready = false;
  s_remote_position.stage = PositionStage::Idle;
  portEXIT_CRITICAL(&s_neighbour_mux);
  s_pending_dx = 0;
  s_pending_dy = 0;
  s_tile_slots_ready = false;
  s_marker_slots_ready = false;
  for (int i = 0; i < kMaxTileSlots; i++) {
    s_tile_slots[i].img = nullptr;
    s_tile_slots[i].ph = nullptr;
    s_tile_slots[i].src[0] = 0;
  }
  s_marker_layer = nullptr;
  s_marker_count = 0;
  s_clusters_valid = false;
  s_cluster_count = 0;
  s_projection_valid = false;
  s_dragging = false;
}
