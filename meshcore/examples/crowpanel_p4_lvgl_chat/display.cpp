// ============================================================
// display.cpp
// ------------------------------------------------------------
// Direct esp_lcd_panel_rgb + LVGL v8 bring-up for the CrowPanel
// DHE04005D (ESP32-P4 + 5" 800x480 ST7262 RGB panel).
//
// LANDSCAPE renders at 792×479 into internal-SRAM LVGL partial buffers
// and blits into the panel FB via esp_lcd_panel_draw_bitmap(). The
// pioarduino / IDF 5.5.4 RGB driver has a fixed -8 column / -1 row
// scan-start offset on this SoC that we couldn't clear via any
// cfg / timing / num_fbs / direct_mode combination; the 792×479 render
// size leaves the FB's leftmost 8 columns and top row as the hardware
// wrap-around region, which stays black because both panel FBs are
// zeroed at init and LVGL never writes there. Visible cost: an 8-px
// black strip on the left and a 1-px black line on top in landscape.
//
// PORTRAIT uses FB #2 as a 480×800 LVGL scratch and ping-pongs FB #0/#1
// as the actual display, with the P4 PPA doing the 90° rotation in
// flush_cb. Portrait doesn't need the crop because the rotated content
// never touches the scan-quirk region.
// ============================================================

#include "display.h"
#include "board_config.h"
#include "gt911.h"
#include "app_globals.h"
#include "utils.h"
#include "ui_homescreen.h"
#include "repeater_ui.h"

#include <cstring>

#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_rgb.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <driver/ppa.h>

#include <limits.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

extern int16_t SCR_H;   // set by init_layout_constants()
extern bool g_landscape_mode;

// Screens that own their own drag gestures (map-pan, long settings
// scroll) should never surrender a drag to the global swipe-back /
// swipe-home handler. touch_read_cb() checks the active screen
// against these pointers and skips swipe detection when one is on.
extern "C" lv_obj_t* ui_mapscreen;
extern "C" lv_obj_t* ui_settingscreen;

static const char* TAG = "display";

static constexpr int kLvTickMs           = 2;
// 32 KB. LVGL widget event dispatch sits on top of fairly deep call
// stacks (lv_timer_handler → lv_event_send → user_callback). Repeater
// action callbacks (Status/Advert/Neighbours/Reboot) further chain into
// MeshCore's createDatagram → Utils::encryptThenMAC, which invokes
// mbedtls AES (~1.5-2 KB locals), and then sendFlood/sendDirect into
// the radio queue path. We were freezing reliably on these click
// handlers at 12 KB; bumping to 20 KB didn't fully fix it (still
// freezing on user reproduction with WiFi off), so lift to 32 KB to
// rule out residual overflow risk. Also pinned to Core 0 below to
// avoid cross-core PSRAM cache coherency on AES DMA buffers.
static constexpr int kLvglTaskStackBytes = 32 * 1024;
static constexpr int kLvglTaskPriority   = 15;
static constexpr int kLvglTaskCore       = 0;
static constexpr int kFrameBufferMax     = 3;

// Landscape logical size — H_size − 8 columns and V_size − 1 rows.
// The omitted right strip and bottom row map (via the -8/-1 scan
// offset) onto panel[0..7, *] and panel[*, 0], which stay black.
static constexpr int kLandWidth          = H_size - 8;   // 792
static constexpr int kLandHeight         = V_size - 1;   // 479
// LVGL landscape partial-render height. Bumped 20 → 40 after A/B
// tuning (20 → 32 → 40 → 48 → 64). Each jump up to 40 cut per-flush
// overhead (LVGL dispatch + esp_lcd_panel_draw_bitmap DMA kick)
// measurably; past 40 returns were not worth the extra ~25-75 KB
// internal SRAM they'd lock out from ESP-Hosted WiFi + mbedtls.
// 50% fewer flushes per full-screen redraw vs 20 rows. Costs
// ~63 KB more internal SRAM vs baseline for the two ping-pong
// buffers (792 × 40 × 2 = 63,360 B each, up from 31,680 B).
static constexpr int kLandDrawRows       = 40;
// Portrait logical size — mirror of the landscape compensation across
// the 90° rotation. LVGL sees a 479×792 viewport (1 column less than
// V_size, 8 rows less than H_size). The unwritten FB borders (col 0
// in the bottom 8-row strip, and row 479 in the rightmost column)
// stay black — the scan offset wraps them out of the visible panel
// area. Without this, the bottom 8 LVGL rows (the last keyboard row)
// wrap to the top edge of the physical screen and the rightmost LVGL
// column wraps to the opposite edge.
static constexpr int kPortWidth          = V_size - 1;   // 479
static constexpr int kPortHeight         = H_size - 8;   // 792
// LVGL portrait partial-render height. Bumped 20 → 40 after A/B
// tuning (20 → 32 → 40 → 48 → 64). Each jump up to 40 amortised
// more UI work per PPA SRM rotation in flush_cb (setup + blocking
// wait) measurably; past 40 returns flatlined. 50% fewer flushes
// per full-screen redraw vs 20 rows. Costs ~38 KB more internal
// SRAM for the two ping-pong buffers (479 × 40 × 2 = 38,320 B
// each, up from 19,160 B).
static constexpr int kPortDrawRows       = 40;

static esp_lcd_panel_handle_t s_panel                       = nullptr;
static lv_disp_draw_buf_t     s_draw_buf;
static lv_disp_drv_t          s_disp_drv;
static lv_color_t*            s_panel_fbs[kFrameBufferMax] = {};
static size_t                 s_panel_fb_count             = 0;
static ppa_client_handle_t    s_ppa_srm                    = nullptr;
// Binary semaphore signalled by the PPA on_trans_done callback. We
// drive PPA in NON_BLOCKING mode and wait on this semaphore with a
// timeout, so a missed/lost done-interrupt drops a frame instead of
// freezing the LVGL task forever (the failure mode that produced the
// portrait-only repeater-button freeze: ppa_do_scale_rotate_mirror in
// BLOCKING mode used portMAX_DELAY internally and never returned).
static SemaphoreHandle_t      s_ppa_done_sem               = nullptr;
static SemaphoreHandle_t      s_lvgl_mux                   = nullptr;
static esp_timer_handle_t     s_tick_timer                 = nullptr;
static TaskHandle_t           s_lvgl_task                  = nullptr;
static int                    s_portrait_present_fb_idx    = 0;

static lv_indev_drv_t         s_indev_drv;
static bool                   s_touch_ready = false;

extern volatile bool g_ppa_recovery_redraw_pending;

// PPA → CPU rotation fallback. After this many consecutive PPA
// failures (timeout or ESP_FAIL even after a client reset), we
// permanently disable PPA SRM for the rest of the boot and do the
// 90° rotation on the CPU. Slower but rock-solid.
static constexpr uint32_t kPpaConsecutiveFailLimit = 3;
static volatile uint32_t  s_ppa_consecutive_fails  = 0;
volatile bool             g_ppa_disabled           = false;

// Software 90° CCW rotation of `src` (rect_w × rect_h) into
// scan_fb at panel offset (dst_x, dst_y). After rotation, the
// rect appears as rect_h px wide × rect_w px tall in the output.
//
// LVGL coord (lx, ly) within the source rect maps to panel coord:
//   panel_x = dst_x + ly
//   panel_y = dst_y + (rect_w - 1 - lx)
//
// Walk the source row-by-row; this is the cache-friendly direction
// for the source (which is in internal SRAM after LVGL renders it).
// The destination writes are scattered (one column per source row)
// but that's unavoidable for a 90° rotation.
static void rotate90_ccw_cpu(const lv_color_t* src, int rect_w, int rect_h,
                             lv_color_t* dst_fb, int dst_x, int dst_y,
                             int dst_pitch) {
  for (int sy = 0; sy < rect_h; ++sy) {
    int panel_x = dst_x + sy;
    const lv_color_t* srow = src + sy * rect_w;
    for (int sx = 0; sx < rect_w; ++sx) {
      int panel_y = dst_y + (rect_w - 1 - sx);
      dst_fb[panel_y * dst_pitch + panel_x] = srow[sx];
    }
  }
}

// PPA done callback — runs in ISR context. Signals the binary
// semaphore the flush_cb is waiting on. Returning true asks the IDF
// to issue a yield-from-ISR if a higher-priority task was woken.
//
// Null-guarded: if we get called after the semaphore was deleted
// during a botched re-register sequence, we'd hard-fault the ISR
// without this check.
static bool ppa_trans_done_cb(ppa_client_handle_t /*client*/,
                              ppa_event_data_t* /*event_data*/,
                              void* /*user_data*/) {
  if (!s_ppa_done_sem) return false;
  BaseType_t hpw = pdFALSE;
  xSemaphoreGiveFromISR(s_ppa_done_sem, &hpw);
  return hpw == pdTRUE;
}

static void flush_cb(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* px) {
  if (g_landscape_mode) {
    // LVGL is 792×479 and writes into FB[0..791, 0..478]. The RGB
    // peripheral's -8/-1 scan offset maps panel[8, 1] onto FB[0, 0]
    // for free, so no coordinate adjustment is needed here. The
    // untouched FB regions (columns 792..799 and row 479) were zeroed
    // during display_init() and stay black — that's what the scan
    // shows on panel[0..7, *] and panel[*, 0].
    esp_err_t err = esp_lcd_panel_draw_bitmap(
        s_panel,
        area->x1, area->y1,
        area->x2 + 1, area->y2 + 1,
        px);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Landscape partial flush failed: %d", (int)err);
    }
    lv_disp_flush_ready(drv);
    return;
  }

  // Portrait anti-tear mode:
  // LVGL renders full frames (full_refresh=1) in 479x792 logical space.
  // We rotate the full frame into the inactive RGB panel framebuffer and
  // present it in one shot via draw_bitmap(), so scanout sees complete
  // frames instead of in-place partial updates.
  //
  // Coord mapping for 90° CCW from 479×792 logical to 800×480 physical:
  //   LVGL (x, y) → FB[kPortWidth-1 - x, y]  = FB[478 - x, y]
  // The compensation matches the landscape -8/-1 scan-offset: LVGL y=0
  // lands at FB column 0 (which wraps out of view) only if LVGL y could
  // reach 792..799 — but we constrained ver_res to kPortHeight=792 so
  // that never happens. Similarly FB row 479 (which wraps to the
  // opposite panel edge) is avoided because LVGL x maxes at
  // kPortWidth-1 = 478, mapping to FB row 0, never to row 479.
  //
  // A rect (x1,y1)-(x2,y2) of size (wx × wy) maps to a panel-space rect
  // of size (wy × wx) with top-left = (y1, kPortWidth - 1 - x2).
  // Use the non-active FB as render target for this present pass.
  int target_fb_idx = (s_portrait_present_fb_idx == 0) ? 1 : 0;
  if (target_fb_idx >= (int)s_panel_fb_count) target_fb_idx = 0;
  lv_color_t* scan_fb = s_panel_fbs[target_fb_idx];
  if (!scan_fb) {
    lv_disp_flush_ready(drv);
    return;
  }

  // In full-refresh mode the logical frame is kPortWidth x kPortHeight.
  const int rect_w = kPortWidth;
  const int rect_h = kPortHeight;
  if (rect_w <= 0 || rect_h <= 0) {
    lv_disp_flush_ready(drv);
    return;
  }

  // CPU rotation fallback path — taken after the PPA hardware proved
  // unreliable (kPpaConsecutiveFailLimit consecutive failures, or
  // PPA was never available). Slower but never wedges.
  if (g_ppa_disabled || !s_ppa_srm || !s_ppa_done_sem) {
    rotate90_ccw_cpu(px, rect_w, rect_h,
                     scan_fb,
                     0,                               // dst_x
                     0,                               // dst_y
                     H_size);
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, H_size, V_size, scan_fb);
    s_portrait_present_fb_idx = target_fb_idx;
    lv_disp_flush_ready(drv);
    return;
  }

  ppa_srm_oper_config_t oper = {};
  oper.in.buffer          = px;
  oper.in.pic_w           = rect_w;
  oper.in.pic_h           = rect_h;
  oper.in.block_w         = rect_w;
  oper.in.block_h         = rect_h;
  oper.in.block_offset_x  = 0;
  oper.in.block_offset_y  = 0;
  oper.in.srm_cm          = PPA_SRM_COLOR_MODE_RGB565;
  oper.out.buffer         = scan_fb;
  oper.out.buffer_size    = H_size * V_size * sizeof(lv_color_t);
  oper.out.pic_w          = H_size;      // panel width (800)
  oper.out.pic_h          = V_size;      // panel height (480)
  oper.out.block_offset_x = 0;
  oper.out.block_offset_y = 0;
  oper.out.srm_cm         = PPA_SRM_COLOR_MODE_RGB565;
  oper.rotation_angle     = PPA_SRM_ROTATION_ANGLE_90;
  oper.scale_x            = 1.0f;
  oper.scale_y            = 1.0f;
  oper.mirror_x           = false;
  oper.mirror_y           = false;
  oper.rgb_swap           = false;
  oper.byte_swap          = false;
  oper.alpha_update_mode  = PPA_ALPHA_NO_CHANGE;
  oper.mode               = PPA_TRANS_MODE_NON_BLOCKING;
  oper.user_data          = nullptr;

  esp_err_t r = ppa_do_scale_rotate_mirror(s_ppa_srm, &oper);
  bool need_reset = false;
  bool fallback_render = false;
  if (r == ESP_OK) {
    // NON_BLOCKING: the call returned immediately after queuing the
    // op. Wait for our on_trans_done callback to give the semaphore.
    // 80 ms timeout: a missed PPA done-irq would otherwise wedge
    // lvgl_task forever (the original portrait freeze symptom).
    if (xSemaphoreTake(s_ppa_done_sem, pdMS_TO_TICKS(80)) != pdTRUE) {
      ESP_LOGE(TAG, "PPA done timeout — frame dropped, resetting PPA");
      need_reset = true;
      fallback_render = true;
    } else {
      s_ppa_consecutive_fails = 0;
    }
  } else if (r == ESP_FAIL) {
    // Queue full — previous transaction timed out and is still
    // occupying the (max_pending_trans_num=1) slot. Reset clears it.
    need_reset = true;
    fallback_render = true;
  } else {
    ESP_LOGE(TAG, "PPA rotate (partial) failed: %d rect=(%d,%d)-(%d,%d)",
             (int)r, (int)area->x1, (int)area->y1, (int)area->x2, (int)area->y2);
    fallback_render = true;
  }

  if (need_reset) {
    s_ppa_consecutive_fails++;
    if (s_ppa_consecutive_fails >= kPpaConsecutiveFailLimit) {
      // Three resets in a row — the hardware is fundamentally unhappy
      // with our SRM workload. Switch to CPU rotation permanently for
      // the rest of this boot.
      ESP_LOGE(TAG, "PPA failed %u times consecutively — switching to CPU rotation",
               (unsigned)s_ppa_consecutive_fails);
      ppa_unregister_client(s_ppa_srm);
      s_ppa_srm = nullptr;
      g_ppa_disabled = true;
      g_ppa_recovery_redraw_pending = true;
    } else {
      // Hardware reset: unregister + re-register the client so
      // subsequent flushes start with an empty transaction queue.
      esp_err_t e1 = ppa_unregister_client(s_ppa_srm);
      s_ppa_srm = nullptr;
      ppa_client_config_t ppa_cfg = {};
      ppa_cfg.oper_type             = PPA_OPERATION_SRM;
      ppa_cfg.max_pending_trans_num = 1;
      esp_err_t e2 = ppa_register_client(&ppa_cfg, &s_ppa_srm);
      esp_err_t e3 = ESP_OK;
      if (e2 == ESP_OK && s_ppa_srm) {
        ppa_event_callbacks_t cbs = {};
        cbs.on_trans_done = ppa_trans_done_cb;
        e3 = ppa_client_register_event_callbacks(s_ppa_srm, &cbs);
        // Drain any stale "give" queued on the semaphore between
        // the timeout and the unregister.
        xSemaphoreTake(s_ppa_done_sem, 0);
      }
      if (e2 != ESP_OK || !s_ppa_srm || e3 != ESP_OK) {
        ESP_LOGE(TAG, "PPA reset failed (unreg=%d reg=%d cb=%d) — switching to CPU rotation",
                 (int)e1, (int)e2, (int)e3);
        if (s_ppa_srm) ppa_unregister_client(s_ppa_srm);
        s_ppa_srm = nullptr;
        g_ppa_disabled = true;
      }
      g_ppa_recovery_redraw_pending = true;
    }
  }

  // CPU-render this frame whenever the PPA path failed, so the user
  // sees fresh content immediately rather than waiting for the next
  // dirty rect after recovery.
  if (fallback_render) {
    rotate90_ccw_cpu(px, rect_w, rect_h,
                     scan_fb,
                     0,
                     0,
                     H_size);
  }
  esp_lcd_panel_draw_bitmap(s_panel, 0, 0, H_size, V_size, scan_fb);
  s_portrait_present_fb_idx = target_fb_idx;

  lv_disp_flush_ready(drv);
}

static void lv_tick_cb(void*) {
  lv_tick_inc(kLvTickMs);
}

// Set inside flush_cb after a PPA reset. Main loop picks it up under
// lvgl_lock and invalidates the active screen so we get a clean redraw
// to replace whatever frozen state the FB was left in.
volatile bool g_ppa_recovery_redraw_pending = false;

static void lvgl_task(void*) {
  ESP_LOGI(TAG, "LVGL task started");
  while (true) {
    uint32_t next_ms = kLvTickMs;
    if (xSemaphoreTakeRecursive(s_lvgl_mux, portMAX_DELAY) == pdTRUE) {
      next_ms = lv_timer_handler();
      xSemaphoreGiveRecursive(s_lvgl_mux);
    }
    if (next_ms < (uint32_t)kLvTickMs) next_ms = kLvTickMs;
    if (next_ms > 10) next_ms = 10;
    vTaskDelay(pdMS_TO_TICKS(next_ms));
  }
}

bool display_init(void) {
  ESP_LOGI(TAG, "esp_lcd_new_rgb_panel()");

  esp_lcd_rgb_panel_config_t cfg = {};
  cfg.data_width       = 16;
  cfg.bits_per_pixel   = 16;
  cfg.clk_src          = LCD_CLK_SRC_DEFAULT;
  cfg.disp_gpio_num    = RGB_PIN_NUM_DISP_EN;
  cfg.pclk_gpio_num    = RGB_PIN_NUM_PCLK;
  cfg.vsync_gpio_num   = RGB_PIN_NUM_VSYNC;
  cfg.hsync_gpio_num   = RGB_PIN_NUM_HSYNC;
  cfg.de_gpio_num      = RGB_PIN_NUM_DE;
  cfg.data_gpio_nums[0]  = RGB_PIN_NUM_DATA0;
  cfg.data_gpio_nums[1]  = RGB_PIN_NUM_DATA1;
  cfg.data_gpio_nums[2]  = RGB_PIN_NUM_DATA2;
  cfg.data_gpio_nums[3]  = RGB_PIN_NUM_DATA3;
  cfg.data_gpio_nums[4]  = RGB_PIN_NUM_DATA4;
  cfg.data_gpio_nums[5]  = RGB_PIN_NUM_DATA5;
  cfg.data_gpio_nums[6]  = RGB_PIN_NUM_DATA6;
  cfg.data_gpio_nums[7]  = RGB_PIN_NUM_DATA7;
  cfg.data_gpio_nums[8]  = RGB_PIN_NUM_DATA8;
  cfg.data_gpio_nums[9]  = RGB_PIN_NUM_DATA9;
  cfg.data_gpio_nums[10] = RGB_PIN_NUM_DATA10;
  cfg.data_gpio_nums[11] = RGB_PIN_NUM_DATA11;
  cfg.data_gpio_nums[12] = RGB_PIN_NUM_DATA12;
  cfg.data_gpio_nums[13] = RGB_PIN_NUM_DATA13;
  cfg.data_gpio_nums[14] = RGB_PIN_NUM_DATA14;
  cfg.data_gpio_nums[15] = RGB_PIN_NUM_DATA15;

  cfg.timings.pclk_hz           = LCD_CLK_MHZ * 1000 * 1000;
  cfg.timings.h_res             = H_size;
  cfg.timings.v_res             = V_size;
  cfg.timings.hsync_pulse_width = LCD_HPW;
  cfg.timings.hsync_back_porch  = LCD_HBP;
  cfg.timings.hsync_front_porch = LCD_HFP;
  cfg.timings.vsync_pulse_width = LCD_VPW;
  cfg.timings.vsync_back_porch  = LCD_VBP;
  cfg.timings.vsync_front_porch = LCD_VFP;
  cfg.timings.flags.hsync_idle_low  = 0;
  cfg.timings.flags.vsync_idle_low  = 0;
  cfg.timings.flags.de_idle_high    = 0;
  cfg.timings.flags.pclk_active_neg = 1;
  cfg.timings.flags.pclk_idle_high  = 1;
  // Portrait no longer needs ping-pong framebuffers — PPA now writes the
  // rotated dirty rect directly into the single scanning FB on every
  // flush (same write-model landscape uses via esp_lcd_panel_draw_bitmap).
  cfg.num_fbs               = 2;
  // Larger bounce buffers reduce RGB DMA starvation during heavy UI redraws
  // (fast scroll / map pans), which is a common source of tearing bands.
  cfg.bounce_buffer_size_px = H_size * 40;
  cfg.dma_burst_size        = 64;
  cfg.flags.fb_in_psram     = 1;

  esp_err_t err = esp_lcd_new_rgb_panel(&cfg, &s_panel);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_lcd_new_rgb_panel failed: %d", (int)err);
    return false;
  }
  esp_lcd_panel_reset(s_panel);
  esp_lcd_panel_init(s_panel);

  void* raw_fbs[kFrameBufferMax] = {};
  err = esp_lcd_rgb_panel_get_frame_buffer(s_panel, cfg.num_fbs, &raw_fbs[0], &raw_fbs[1], &raw_fbs[2]);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_lcd_rgb_panel_get_frame_buffer failed: %d", (int)err);
    return false;
  }

  s_panel_fb_count = cfg.num_fbs;
  for (size_t i = 0; i < s_panel_fb_count; ++i) {
    s_panel_fbs[i] = static_cast<lv_color_t*>(raw_fbs[i]);
    if (s_panel_fbs[i]) {
      memset(s_panel_fbs[i], 0, H_size * V_size * sizeof(lv_color_t));
    }
  }

  ESP_LOGI(TAG, "lv_init()");
  lv_init();

  const size_t ui_width   = g_landscape_mode ? (size_t)kLandWidth  : (size_t)kPortWidth;
  const size_t ui_height  = g_landscape_mode ? (size_t)kLandHeight : (size_t)kPortHeight;

  if (g_landscape_mode) {
    if (!s_panel_fbs[0] || !s_panel_fbs[1]) {
      ESP_LOGE(TAG, "Landscape framebuffer allocation incomplete");
      return false;
    }
    // RGB anti-tear mode (landscape): LVGL full refresh with two full-screen
    // draw buffers, allocated in PSRAM.
    // This aligns with Espressif's "double-buffer + full refresh" guidance
    // for tearing reduction on RGB scanout panels.
    const size_t land_buf_px = (size_t)kLandWidth * (size_t)kLandHeight;
    const size_t land_buf_sz = land_buf_px * sizeof(lv_color_t);
    lv_color_t* buf_a = (lv_color_t*)heap_caps_aligned_alloc(
        128, land_buf_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    lv_color_t* buf_b = (lv_color_t*)heap_caps_aligned_alloc(
        128, land_buf_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf_a || !buf_b) {
      if (buf_a) heap_caps_free(buf_a);
      if (buf_b) heap_caps_free(buf_b);
      buf_a = (lv_color_t*)heap_caps_aligned_alloc(
          128, land_buf_sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      buf_b = (lv_color_t*)heap_caps_aligned_alloc(
          128, land_buf_sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!buf_a || !buf_b) {
      ESP_LOGE(TAG, "Landscape LVGL buffer alloc failed");
      return false;
    }
    lv_disp_draw_buf_init(&s_draw_buf, buf_a, buf_b, land_buf_px);
    ESP_LOGI(TAG, "LVGL: landscape full-refresh %dx%d (8-px left / 1-row top scan-offset borders)",
             (int)ui_width, (int)ui_height);
  } else {
    if (!s_panel_fbs[0] || !s_panel_fbs[1]) {
      ESP_LOGE(TAG, "Portrait framebuffer allocation incomplete");
      return false;
    }
    // Partial-render buffer preferentially in internal SRAM (fastest
    // path for LVGL rendering). Fall back to PSRAM if internal is
    // fragmented — ESP-Hosted WiFi holds substantial internal SRAM
    // for its SDIO buffers, so in tight-heap scenarios the 19 KB
    // aligned alloc can fail. PSRAM is ~5× slower for random access
    // but doesn't block the flush path, and the PPA DMA reads PSRAM
    // at its own rate regardless. Better slow than dead.
    // Width matches the (scan-offset-compensated) LVGL logical width,
    // so LVGL's flush never straddles the FB border the scan offset
    // wraps out of view.
    const size_t port_buf_px = (size_t)kPortWidth * (size_t)kPortHeight;
    const size_t port_buf_sz = port_buf_px * sizeof(lv_color_t);
    size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t free_psr = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "Portrait buf need=%u internal_free=%u psram_free=%u",
             (unsigned)port_buf_sz, (unsigned)free_int, (unsigned)free_psr);
    lv_color_t* buf_a = (lv_color_t*)heap_caps_aligned_alloc(
        128, port_buf_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    lv_color_t* buf_b = (lv_color_t*)heap_caps_aligned_alloc(
        128, port_buf_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf_a || !buf_b) {
      if (buf_a) heap_caps_free(buf_a);
      if (buf_b) heap_caps_free(buf_b);
      ESP_LOGE(TAG, "Portrait full-frame LVGL buffers require PSRAM");
      return false;
    }
    if (!buf_a || !buf_b) {
      ESP_LOGE(TAG, "Portrait LVGL buffer alloc failed (both SRAM and PSRAM)");
      return false;
    }
    lv_disp_draw_buf_init(&s_draw_buf, buf_a, buf_b, port_buf_px);

    ppa_client_config_t ppa_cfg = {};
    ppa_cfg.oper_type             = PPA_OPERATION_SRM;
    ppa_cfg.max_pending_trans_num = 1;
    esp_err_t ppa_err = ppa_register_client(&ppa_cfg, &s_ppa_srm);
    if (ppa_err != ESP_OK || !s_ppa_srm) {
      ESP_LOGE(TAG, "ppa_register_client failed: %d (handle=%p)",
               (int)ppa_err, (void*)s_ppa_srm);
      // Don't abort display_init — flush_cb has a null-guard on
      // s_ppa_srm and will silently no-op, leaving the panel at its
      // last contents. User will see a frozen screen in portrait
      // rather than a hard crash.
      s_ppa_srm = nullptr;
    } else {
      // Binary semaphore used to escape PPA hangs. Created BEFORE
      // registering the on_trans_done callback so the very first
      // PPA done event has a valid semaphore to give.
      s_ppa_done_sem = xSemaphoreCreateBinary();
      if (!s_ppa_done_sem) {
        ESP_LOGE(TAG, "PPA done-sem allocation failed");
        ppa_unregister_client(s_ppa_srm);
        s_ppa_srm = nullptr;
      } else {
        ppa_event_callbacks_t cbs = {};
        cbs.on_trans_done = ppa_trans_done_cb;
        ppa_err = ppa_client_register_event_callbacks(s_ppa_srm, &cbs);
        if (ppa_err != ESP_OK) {
          ESP_LOGE(TAG, "ppa_client_register_event_callbacks failed: %d", (int)ppa_err);
          // Without the callback, our NON_BLOCKING + xSemaphoreTake
          // pattern would always time out. Bail to the null-handle
          // path instead so flush_cb skips the PPA call entirely.
          ppa_unregister_client(s_ppa_srm);
          vSemaphoreDelete(s_ppa_done_sem);
          s_ppa_srm = nullptr;
          s_ppa_done_sem = nullptr;
        }
      }
    }

    ESP_LOGI(TAG, "LVGL: portrait full-refresh %dx%d + frame-present rotation (ppa=%p)",
             (int)ui_width, (int)ui_height, (void*)s_ppa_srm);
  }

  lv_disp_drv_init(&s_disp_drv);
  s_disp_drv.hor_res      = (lv_coord_t)ui_width;
  s_disp_drv.ver_res      = (lv_coord_t)ui_height;
  // Landscape: partial refresh for speed (avoid full-frame redraw on every
  // small invalidation). Portrait currently keeps full-refresh because the
  // PPA rotation path is full-frame based in flush_cb.
  s_disp_drv.full_refresh = g_landscape_mode ? 0 : 1;
  s_disp_drv.sw_rotate    = 0;
  s_disp_drv.rotated      = LV_DISP_ROT_NONE;
  s_disp_drv.flush_cb     = flush_cb;
  s_disp_drv.draw_buf     = &s_draw_buf;
  lv_disp_drv_register(&s_disp_drv);

  const esp_timer_create_args_t ta = {
    .callback        = &lv_tick_cb,
    .arg             = nullptr,
    .dispatch_method = ESP_TIMER_TASK,
    .name            = "lv_tick",
    .skip_unhandled_events = true,
  };
  ESP_ERROR_CHECK(esp_timer_create(&ta, &s_tick_timer));
  ESP_ERROR_CHECK(esp_timer_start_periodic(s_tick_timer, kLvTickMs * 1000));

  s_lvgl_mux = xSemaphoreCreateRecursiveMutex();

  ESP_LOGI(TAG, "display_init complete");
  return true;
}

void display_start_lvgl_task(void) {
  if (!s_lvgl_mux) {
    ESP_LOGE(TAG, "LVGL mutex missing - display_init failed?");
    return;
  }
  if (s_lvgl_task) return;

  // Pin LVGL to Core 0. xTaskCreate alone leaves FreeRTOS free to
  // migrate the task across cores, which on the dual-core P4 means
  // mbedtls AES DMA buffers (PSRAM-backed) can be touched by two
  // different cache lines — we hit infrequent visual corruption +
  // occasional hard freezes traced to that pattern. Pinning makes
  // the cache coherency story trivial.
  BaseType_t ok = xTaskCreatePinnedToCore(
      lvgl_task, "lvgl", kLvglTaskStackBytes, nullptr, kLvglTaskPriority,
      &s_lvgl_task, kLvglTaskCore);
  if (ok != pdPASS) {
    s_lvgl_task = nullptr;
    ESP_LOGE(TAG, "Failed to start LVGL task");
    return;
  }

  ESP_LOGI(TAG, "Dedicated LVGL task started");
}

// Called from the main-loop deferred-work block. Picks up the
// flag set by flush_cb after a PPA reset and invalidates the
// active screen so LVGL re-renders everything in the next frame.
// Caller is expected to hold s_lvgl_mux (i.e. inside an lvgl_lock
// block).
void display_handle_ppa_recovery_if_pending(void) {
  if (!g_ppa_recovery_redraw_pending) return;
  g_ppa_recovery_redraw_pending = false;
  lv_obj_t* scr = lv_scr_act();
  if (scr) lv_obj_invalidate(scr);
}

bool lvgl_lock(int timeout_ms) {
  if (!s_lvgl_mux) return false;
  TickType_t t = (timeout_ms < 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
  return xSemaphoreTakeRecursive(s_lvgl_mux, t) == pdTRUE;
}

void lvgl_unlock(void) {
  if (s_lvgl_mux) xSemaphoreGiveRecursive(s_lvgl_mux);
}

// ------------------------------------------------------------
// GT911 touch - LVGL input device with v11 gesture logic
// ------------------------------------------------------------
// Ported from v11's display.cpp::touch_cb():
//   * Wake the screen on any press, swallow that press so the widget
//     underneath doesn't receive a click.
//   * Track swipe start/end. On release, if no keyboard is visible:
//       - horizontal left swipe (>80 px, small dy) -> deferred back
//       - vertical up swipe (>80 px, small dx) starting near the bottom
//         edge -> deferred home
//   * If the on-screen keyboard is visible in a chat, a press that lands
//     outside the keyboard AND outside the text-input -> dismiss keyboard.
//   * Throttled last-touch activity tracking (for the dim/sleep timer in
//     loop()).
static void touch_read_cb(lv_indev_drv_t* drv, lv_indev_data_t* data) {
  (void)drv;

  uint16_t rx = 0, ry = 0;
  bool pressed = gt911_read(&rx, &ry);

  // GT911 always reports in landscape panel-space (0..H_size x 0..V_size).
  // In landscape the UI occupies panel[8..799, 1..479] (see the 8-px
  // left / 1-row top scan-offset borders in flush_cb), so subtract the
  // offset and clamp into the 792×479 LVGL space. Portrait applies the
  // inverse of the 90-degree CCW rotate used in flush_cb.
  uint16_t x, y;
  if (g_landscape_mode) {
    int16_t xs = (int16_t)rx - 8;
    int16_t ys = (int16_t)ry - 1;
    if (xs < 0) xs = 0;
    if (ys < 0) ys = 0;
    if (xs > kLandWidth  - 1) xs = kLandWidth  - 1;
    if (ys > kLandHeight - 1) ys = kLandHeight - 1;
    x = (uint16_t)xs;
    y = (uint16_t)ys;
  } else {
    // Portrait: raw (rx, ry) are panel-native coords (0..799, 0..479).
    // Forward rendering maps LVGL (lx, ly) → panel (ly+8, 479-lx), so
    // the touch inverse is: lx = (V_size-1) - ry, ly = rx - 8.
    // Clamp into the 479×792 LVGL logical space — touches on the
    // wrap-strip (panel cols 0..7 or row 0) clamp to the nearest edge.
    int16_t xs = (int16_t)(V_size - 1) - (int16_t)ry;
    int16_t ys = (int16_t)rx - 8;
    if (xs < 0) xs = 0;
    if (ys < 0) ys = 0;
    if (xs > kPortWidth  - 1) xs = kPortWidth  - 1;
    if (ys > kPortHeight - 1) ys = kPortHeight - 1;
    x = (uint16_t)xs;
    y = (uint16_t)ys;
  }

  static uint16_t s_last_x = 0, s_last_y = 0;
  if (pressed) {
    s_last_x = x;
    s_last_y = y;
  } else {
    x = s_last_x;
    y = s_last_y;
  }

  if (!g_screen_awake && pressed) {
    note_touch_activity();
    g_swallow_touch   = true;
    g_touch_was_press = false;
    data->state = LV_INDEV_STATE_RELEASED;
    return;
  }
  if (g_swallow_touch) {
    if (!pressed) g_swallow_touch = false;
    data->state = LV_INDEV_STATE_RELEASED;
    return;
  }

  if (pressed && !g_touch_was_press) {
    g_swipe_start_x = x;
    g_swipe_start_y = y;
    g_swipe_tracking = true;
  }
  if (!pressed && g_touch_was_press && g_swipe_tracking) {
    g_swipe_tracking = false;
    bool kb_visible = (ui_Keyboard1 && !lv_obj_has_flag(ui_Keyboard1, LV_OBJ_FLAG_HIDDEN));
    // Skip swipe detection on screens that own their own drag
    // interactions — the map pan, and the vertical scroll on the
    // long settings form. Without this, a horizontal drag on the
    // map would both pan AND trigger swipe-back, which ends up
    // yanking the user off the map mid-pan.
    lv_obj_t* active_screen = lv_scr_act();
    const bool swipe_sensitive_screen =
        (ui_mapscreen     && active_screen == ui_mapscreen)    ||
        (ui_settingscreen && active_screen == ui_settingscreen);

    // Keyboard-visible gating: allow swipe when the gesture STARTED
    // above the keyboard's top edge. Typing drags still get filtered
    // (they start on the keyboard), but a swipe-from-chat-area to
    // exit the chat goes through. Without this, swipe-to-exit was
    // impossible once the keyboard was shown in chat mode.
    bool kb_blocks_swipe = false;
    if (kb_visible) {
      lv_area_t a;
      lv_obj_get_coords(ui_Keyboard1, &a);
      // Only block if the touch-down was inside (or below) the
      // keyboard rect. Touches that started above it are navigation.
      if ((int)g_swipe_start_y >= (int)a.y1) {
        kb_blocks_swipe = true;
      }
    }

    if (!kb_blocks_swipe && !swipe_sensitive_screen) {
      int16_t dx = (int16_t)x - (int16_t)g_swipe_start_x;
      int16_t dy = (int16_t)y - (int16_t)g_swipe_start_y;
      if (dx < -80 && dy > -60 && dy < 60) {
        g_deferred_swipe_back = true;
      }
      if (dy < -80 && dx > -60 && dx < 60 && g_swipe_start_y > (SCR_H - 100)) {
        g_deferred_swipe_home = true;
      }
    }
  }

  if (pressed && !g_touch_was_press) {
    if (g_in_chat_mode && ui_Keyboard1 &&
        !lv_obj_has_flag(ui_Keyboard1, LV_OBJ_FLAG_HIDDEN)) {
      bool on_kb = false, on_ta = false;
      {
        lv_area_t a; lv_obj_get_coords(ui_Keyboard1, &a);
        on_kb = (x >= a.x1 && x <= a.x2 && y >= a.y1 && y <= a.y2);
      }
      if (!on_kb && ui_textsendtype) {
        lv_area_t a; lv_obj_get_coords(ui_textsendtype, &a);
        on_ta = (x >= a.x1 && x <= a.x2 && y >= a.y1 && y <= a.y2);
      }
      if (!on_kb && !on_ta) g_dismiss_keyboard = true;
    }
    // Same tap-outside-to-dismiss behaviour for the repeater CLI keyboard.
    repeater_cli_dismiss_kb_if_outside((int)x, (int)y);
  }

  g_touch_was_press = pressed;

  data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
  data->point.x = x;
  data->point.y = y;

  if (pressed && (millis() - g_last_touch_ms) > 1000) note_touch_activity();
}

bool display_touch_attach(void) {
  if (s_touch_ready) return true;

  if (!gt911_init(H_size, V_size)) {
    ESP_LOGE(TAG, "gt911_init failed - touch disabled");
    return false;
  }

  if (!lvgl_lock(-1)) return false;
  lv_indev_drv_init(&s_indev_drv);
  s_indev_drv.type    = LV_INDEV_TYPE_POINTER;
  s_indev_drv.read_cb = touch_read_cb;
  lv_indev_drv_register(&s_indev_drv);
  lvgl_unlock();

  s_touch_ready = true;
  ESP_LOGI(TAG, "LVGL touch indev registered");
  return true;
}
