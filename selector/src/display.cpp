// ============================================================
// display.cpp — selector firmware variant
// ------------------------------------------------------------
// Direct esp_lcd_panel_rgb + LVGL v8 bring-up for the CrowPanel
// DHE04005D (ESP32-P4 + 5" 800x480 ST7262 RGB panel).
//
// Landscape-only. The pioarduino / IDF 5.5.4 RGB driver has a fixed
// -8 column / -1 row scan-start offset on this SoC; we render LVGL at
// 792×479 and the leftmost 8 columns + top row stay black (FB zeroed
// at init). See comments in the meshcore parent for the long story.
// ============================================================

#include "display.h"
#include "board_config.h"
#include "gt911.h"

#include <cstring>

#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_rgb.h>
#include <esp_log.h>
#include <esp_timer.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

static const char* TAG = "display";

static constexpr int kLvTickMs           = 2;
static constexpr int kLvglTaskStackBytes = 12 * 1024;
static constexpr int kLvglTaskPriority   = 15;
static constexpr int kFrameBufferMax     = 2;

static constexpr int kLandWidth          = H_size - 8;   // 792
static constexpr int kLandHeight         = V_size - 1;   // 479
static constexpr int kLandDrawRows       = 40;

static esp_lcd_panel_handle_t s_panel                       = nullptr;
static lv_disp_draw_buf_t     s_draw_buf;
static lv_disp_drv_t          s_disp_drv;
static lv_color_t*            s_panel_fbs[kFrameBufferMax] = {};
static SemaphoreHandle_t      s_lvgl_mux                   = nullptr;
static esp_timer_handle_t     s_tick_timer                 = nullptr;
static TaskHandle_t           s_lvgl_task                  = nullptr;

static lv_indev_drv_t         s_indev_drv;
static bool                   s_touch_ready = false;

static void flush_cb(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* px) {
  esp_err_t err = esp_lcd_panel_draw_bitmap(
      s_panel,
      area->x1, area->y1,
      area->x2 + 1, area->y2 + 1,
      px);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Partial flush failed: %d", (int)err);
  }
  lv_disp_flush_ready(drv);
}

static void lv_tick_cb(void*) {
  lv_tick_inc(kLvTickMs);
}

static void lvgl_task(void*) {
  ESP_LOGI(TAG, "LVGL task started");
  while (true) {
    uint32_t next_ms = kLvTickMs;
    if (xSemaphoreTakeRecursive(s_lvgl_mux, portMAX_DELAY) == pdTRUE) {
      next_ms = lv_timer_handler();
      xSemaphoreGiveRecursive(s_lvgl_mux);
    }
    if (next_ms < (uint32_t)kLvTickMs) next_ms = kLvTickMs;
    if (next_ms > 33) next_ms = 33;
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
  cfg.num_fbs               = 2;
  cfg.bounce_buffer_size_px = H_size * 20;
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
  err = esp_lcd_rgb_panel_get_frame_buffer(s_panel, cfg.num_fbs, &raw_fbs[0], &raw_fbs[1]);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_lcd_rgb_panel_get_frame_buffer failed: %d", (int)err);
    return false;
  }

  for (int i = 0; i < kFrameBufferMax; ++i) {
    s_panel_fbs[i] = static_cast<lv_color_t*>(raw_fbs[i]);
    if (s_panel_fbs[i]) {
      memset(s_panel_fbs[i], 0, H_size * V_size * sizeof(lv_color_t));
    }
  }

  ESP_LOGI(TAG, "lv_init()");
  lv_init();

  if (!s_panel_fbs[0] || !s_panel_fbs[1]) {
    ESP_LOGE(TAG, "Framebuffer allocation incomplete");
    return false;
  }

  const size_t buf_px = (size_t)kLandWidth * (size_t)kLandDrawRows;
  const size_t buf_sz = buf_px * sizeof(lv_color_t);
  lv_color_t* buf_a = (lv_color_t*)heap_caps_aligned_alloc(
      128, buf_sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  lv_color_t* buf_b = (lv_color_t*)heap_caps_aligned_alloc(
      128, buf_sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!buf_a || !buf_b) {
    ESP_LOGE(TAG, "LVGL buffer alloc failed");
    return false;
  }
  lv_disp_draw_buf_init(&s_draw_buf, buf_a, buf_b, buf_px);
  ESP_LOGI(TAG, "LVGL: landscape partial %dx%d (8-px left / 1-row top scan-offset borders)",
           kLandWidth, kLandHeight);

  lv_disp_drv_init(&s_disp_drv);
  s_disp_drv.hor_res      = kLandWidth;
  s_disp_drv.ver_res      = kLandHeight;
  s_disp_drv.full_refresh = 0;
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

  BaseType_t ok = xTaskCreate(
      lvgl_task, "lvgl", kLvglTaskStackBytes, nullptr, kLvglTaskPriority, &s_lvgl_task);
  if (ok != pdPASS) {
    s_lvgl_task = nullptr;
    ESP_LOGE(TAG, "Failed to start LVGL task");
    return;
  }
  ESP_LOGI(TAG, "Dedicated LVGL task started");
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
// GT911 touch — minimal LVGL pointer indev. No swipe/gesture logic
// (the selector has none of the chat screens that need it).
// ------------------------------------------------------------
static void touch_read_cb(lv_indev_drv_t* drv, lv_indev_data_t* data) {
  (void)drv;

  uint16_t rx = 0, ry = 0;
  bool pressed = gt911_read(&rx, &ry);

  // GT911 reports panel-native (0..H_size, 0..V_size). Forward render is
  // panel[8, 1] = LVGL[0, 0], so subtract the offset and clamp into 792×479.
  int16_t xs = (int16_t)rx - 8;
  int16_t ys = (int16_t)ry - 1;
  if (xs < 0) xs = 0;
  if (ys < 0) ys = 0;
  if (xs > kLandWidth  - 1) xs = kLandWidth  - 1;
  if (ys > kLandHeight - 1) ys = kLandHeight - 1;

  static uint16_t s_last_x = 0, s_last_y = 0;
  if (pressed) {
    s_last_x = (uint16_t)xs;
    s_last_y = (uint16_t)ys;
  }

  data->state   = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
  data->point.x = s_last_x;
  data->point.y = s_last_y;
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
