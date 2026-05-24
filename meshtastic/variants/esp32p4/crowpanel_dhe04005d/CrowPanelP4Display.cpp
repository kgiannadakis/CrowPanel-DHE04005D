#include "CrowPanelP4Display.h"
#include "board_config.h"
#include "crowpanel_backlight.h"
#include "gt911.h"
#include "i2c_bus.h"
#include "stc8.h"

#include <cstring>

#include <Arduino.h>

#include <driver/ppa.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_rgb.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

extern "C" void psram_alloc_guard_arm();
#if LV_USE_LODEPNG
extern "C" void lv_lodepng_init(void);
#endif
// PNG decode arena (PngDecodeArena.cpp): reserve a clean PSRAM block for image
// decoding before the framebuffer corrupts the system PSRAM heap, then route
// LVGL's image draw-buf handlers to it after lv_init().
extern "C" void png_decode_arena_reserve(void);
extern "C" void png_decode_arena_install_handlers(void);

namespace mcui { bool landscape_active(); }

namespace crowpanel_p4 {

static const char* TAG = "p4_display";

static constexpr int kPhysWidth   = H_size;
static constexpr int kPhysHeight  = V_size;
static constexpr int kLandWidth   = H_size - 8;
static constexpr int kLandHeight  = V_size - 1;
static constexpr int kPortWidth   = V_size - 1;
static constexpr int kPortHeight  = H_size - 8;

static constexpr int kFrameBufferMax = 2;
static constexpr uint32_t kLvglTickMs = 2;
static constexpr int kLvglTaskStackBytes = 10240;
static constexpr UBaseType_t kLvglTaskPriority = 5;
static constexpr BaseType_t kLvglTaskCore = 0;
static constexpr uint32_t kLvglTaskSleepMaxMs = 6;
static constexpr int kLandDrawRowsPreferred = 24;
static constexpr int kPortDrawRowsPreferred = 32;
static constexpr int kDrawRowsMin = 16;
static constexpr int kDrawRowsStep = 4;
// Keep the RGB bounce buffers small to preserve INTERNAL DMA memory
// for ESP-Hosted + MQTT socket/TCP activity.
static constexpr int kRgbBounceRows = 8;

static esp_lcd_panel_handle_t s_panel  = nullptr;
static lv_color_t*            s_panel_fbs[kFrameBufferMax] = {};
static lv_display_t*          s_disp   = nullptr;
static lv_indev_t*            s_indev  = nullptr;
static SemaphoreHandle_t      s_lvgl_mux = nullptr;
static bool                   s_landscape = true;
static int                    s_lvgl_w = kLandWidth;
static int                    s_lvgl_h = kLandHeight;
static ppa_client_handle_t    s_ppa_srm = nullptr;
static TaskHandle_t           s_lvgl_task = nullptr;

static void flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px)
{
    if (s_landscape) {
        esp_err_t err = esp_lcd_panel_draw_bitmap(
            s_panel,
            area->x1, area->y1,
            area->x2 + 1, area->y2 + 1,
            px);
        if (err != ESP_OK) ESP_LOGE(TAG, "flush(landscape) failed: %d", (int)err);
        lv_display_flush_ready(disp);
        return;
    }

    if (!s_ppa_srm || !s_panel_fbs[0]) {

        lv_display_flush_ready(disp);
        return;
    }

    const int rect_w = area->x2 - area->x1 + 1;
    const int rect_h = area->y2 - area->y1 + 1;

    ppa_srm_oper_config_t oper = {};
    oper.in.buffer          = px;
    oper.in.pic_w           = rect_w;
    oper.in.pic_h           = rect_h;
    oper.in.block_w         = rect_w;
    oper.in.block_h         = rect_h;
    oper.in.block_offset_x  = 0;
    oper.in.block_offset_y  = 0;
    oper.in.srm_cm          = PPA_SRM_COLOR_MODE_RGB565;

    oper.out.buffer         = s_panel_fbs[0];
    oper.out.buffer_size    = (uint32_t)kPhysWidth * (uint32_t)kPhysHeight * sizeof(lv_color_t);
    oper.out.pic_w          = kPhysWidth;
    oper.out.pic_h          = kPhysHeight;

    oper.out.block_offset_x = area->y1;
    oper.out.block_offset_y = kPortWidth - 1 - area->x2;
    oper.out.srm_cm         = PPA_SRM_COLOR_MODE_RGB565;

    oper.rotation_angle     = PPA_SRM_ROTATION_ANGLE_90;
    oper.scale_x            = 1.0f;
    oper.scale_y            = 1.0f;
    oper.mirror_x           = false;
    oper.mirror_y           = false;
    oper.rgb_swap           = false;
    oper.byte_swap          = false;
    oper.alpha_update_mode  = PPA_ALPHA_NO_CHANGE;
    oper.mode               = PPA_TRANS_MODE_BLOCKING;

    esp_err_t err = ppa_do_scale_rotate_mirror(s_ppa_srm, &oper);
    if (err != ESP_OK) ESP_LOGE(TAG, "flush(portrait) PPA failed: %d", (int)err);

    lv_display_flush_ready(disp);
}

static uint32_t lv_tick_cb(void) { return (uint32_t)millis(); }

static void lvgl_task(void*)
{
    ESP_LOGI(TAG, "LVGL service task started on core %d", xPortGetCoreID());
    while (true) {
        uint32_t wait_ms = kLvglTickMs;
        if (lvgl_lock(-1)) {
            wait_ms = lv_timer_handler();
            lvgl_unlock();
        }
        if (wait_ms < kLvglTickMs) wait_ms = kLvglTickMs;
        if (wait_ms > kLvglTaskSleepMaxMs) wait_ms = kLvglTaskSleepMaxMs;
        vTaskDelay(pdMS_TO_TICKS(wait_ms));
    }
}

static void touch_read_cb(lv_indev_t* indev, lv_indev_data_t* data)
{
    (void)indev;
    uint16_t rx = 0, ry = 0;
    bool pressed = gt911_read(&rx, &ry);
    if (!pressed) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    backlight_notify_activity();
    if (!backlight_is_screen_on()) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    int16_t lx, ly;
    if (s_landscape) {

        lx = (int16_t)rx - 8;
        ly = (int16_t)ry - 1;
    } else {

        lx = (int16_t)(kPortWidth - 1) - (int16_t)ry;
        ly = (int16_t)rx;
    }
    if (lx < 0) lx = 0;
    if (ly < 0) ly = 0;
    if (lx >= s_lvgl_w) lx = s_lvgl_w - 1;
    if (ly >= s_lvgl_h) ly = s_lvgl_h - 1;
    data->point.x = lx;
    data->point.y = ly;
    data->state   = LV_INDEV_STATE_PRESSED;
}

bool display_init()
{
    if (s_disp) {
        ESP_LOGW(TAG, "display_init called twice - ignoring");
        return true;
    }

    s_landscape = mcui::landscape_active();
    s_lvgl_w    = s_landscape ? kLandWidth  : kPortWidth;
    s_lvgl_h    = s_landscape ? kLandHeight : kPortHeight;
    ESP_LOGI(TAG, "orientation=%s, LVGL=%dx%d (panel scans %dx%d)",
             s_landscape ? "landscape" : "portrait",
             s_lvgl_w, s_lvgl_h, kPhysWidth, kPhysHeight);

    ESP_LOGI(TAG, "Bring up I2C1 + STC8 backlight");
    if (!i2c1_bus_handle()) {
        ESP_LOGE(TAG, "i2c1 bus init failed");
        return false;
    }
    stc8_set_pwm_duty(STC8_PWM_LCD_BL_EN, 80);

    const bool touch_ok = gt911_init(H_size, V_size);
    if (!touch_ok) {
        ESP_LOGW(TAG, "gt911_init failed - touch disabled");
    }

    // Tune LVGL draw rows per orientation:
    // - landscape: moderate rows to keep ESP-Hosted headroom
    // - portrait: wider logical height benefits from a larger partial window
    int draw_rows = s_landscape ? kLandDrawRowsPreferred : kPortDrawRowsPreferred;
    size_t buf_sz = 0;
    void* lvgl_buf_a = nullptr;
    void* lvgl_buf_b = nullptr;
    bool lvgl_bufs_in_psram = false;

    // Prefer PSRAM buffers on CrowPanel so INTERNAL DMA-capable memory remains
    // available for WiFi hosted transport and AES/GDMA descriptor allocations.
    // If a given row count does not fit, step down until minimum safe window.
    for (int rows = draw_rows; rows >= kDrawRowsMin; rows -= kDrawRowsStep) {
        const size_t try_px = (size_t)s_lvgl_w * (size_t)rows;
        const size_t try_sz = try_px * sizeof(lv_color_t);
        void* a = heap_caps_aligned_alloc(128, try_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        void* b = heap_caps_aligned_alloc(128, try_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (a && b) {
            lvgl_buf_a = a;
            lvgl_buf_b = b;
            lvgl_bufs_in_psram = true;
            draw_rows = rows;
            buf_sz = try_sz;
            break;
        }
        if (a) heap_caps_free(a);
        if (b) heap_caps_free(b);
    }

    // Fallback to INTERNAL only if PSRAM could not satisfy minimum rows.
    if (!lvgl_buf_a || !lvgl_buf_b) {
        lvgl_bufs_in_psram = false;
        for (int rows = draw_rows; rows >= kDrawRowsMin; rows -= kDrawRowsStep) {
            const size_t try_px = (size_t)s_lvgl_w * (size_t)rows;
            const size_t try_sz = try_px * sizeof(lv_color_t);
            void* a = heap_caps_aligned_alloc(128, try_sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            void* b = heap_caps_aligned_alloc(128, try_sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            if (a && b) {
                lvgl_buf_a = a;
                lvgl_buf_b = b;
                draw_rows = rows;
                buf_sz = try_sz;
                break;
            }
            if (a) heap_caps_free(a);
            if (b) heap_caps_free(b);
        }
    }

    if (!lvgl_buf_a || !lvgl_buf_b) {
        ESP_LOGE(TAG, "lvgl buf alloc failed");
        return false;
    }

    ESP_LOGI(TAG, "LVGL draw rows=%u, width=%d (%s)", (unsigned)draw_rows, s_lvgl_w,
             lvgl_bufs_in_psram ? "PSRAM" : "INTERNAL");

    // Reserve the image-decode PSRAM arena now, while the system PSRAM heap is
    // still clean. esp_lcd_panel_init() (below) corrupts that heap's free-list,
    // after which runtime PSRAM allocations assert. The arena's private
    // multi_heap is unaffected because its memory is reserved here, up-front.
    png_decode_arena_reserve();

    if (!s_landscape) {
        ESP_LOGI(TAG, "ppa_register_client() (pre-FB)");
        ppa_client_config_t ppa_cfg = {};
        ppa_cfg.oper_type             = PPA_OPERATION_SRM;
        ppa_cfg.max_pending_trans_num = 1;
        esp_err_t pe = ppa_register_client(&ppa_cfg, &s_ppa_srm);
        if (pe != ESP_OK || !s_ppa_srm) {
            ESP_LOGE(TAG, "ppa_register_client failed: %d - falling back to landscape", (int)pe);
            s_ppa_srm   = nullptr;
            s_landscape = true;
            s_lvgl_w    = kLandWidth;
            s_lvgl_h    = kLandHeight;
        }
    }

    // esp_lcd_new_rgb_panel() permanently corrupts the PSRAM TLSF heap on
    // this P4 board. Block until main.cpp's early-network init has brought
    // WiFi up and opened the MQTT socket, so every lwIP/ESP-Hosted PSRAM
    // allocation lands on the still-clean heap. Backstop timeout keeps a
    // stalled main thread from leaving the screen dark forever.
    ESP_LOGI(TAG, "FB gate: waiting for network prewarm");
    crowpanel_p4_fb_gate_wait(45000);

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
    cfg.timings.h_res             = kPhysWidth;
    cfg.timings.v_res             = kPhysHeight;
    cfg.timings.hsync_pulse_width = LCD_HPW;
    cfg.timings.hsync_back_porch  = LCD_HBP;
    cfg.timings.hsync_front_porch = LCD_HFP;
    cfg.timings.vsync_pulse_width = LCD_VPW;
    cfg.timings.vsync_back_porch  = LCD_VBP;
    cfg.timings.vsync_front_porch = LCD_VFP;
    cfg.timings.flags.pclk_active_neg = 1;
    cfg.timings.flags.pclk_idle_high  = 1;

    // Single FB is more memory-stable on P4 when WiFi/MQTT comes up.
    cfg.num_fbs               = 1;
    cfg.bounce_buffer_size_px = kPhysWidth * kRgbBounceRows;
    cfg.dma_burst_size        = 64;
    cfg.flags.fb_in_psram     = 1;

    esp_err_t err = esp_lcd_new_rgb_panel(&cfg, &s_panel);
    if (err != ESP_OK) { ESP_LOGE(TAG, "rgb_panel: %d", err); return false; }
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);

    void* raw_fbs[kFrameBufferMax] = {};
    err = esp_lcd_rgb_panel_get_frame_buffer(s_panel, 1, &raw_fbs[0]);
    if (err != ESP_OK) { ESP_LOGE(TAG, "get_fb: %d", err); return false; }
    for (size_t i = 0; i < kFrameBufferMax; ++i) {
        s_panel_fbs[i] = static_cast<lv_color_t*>(raw_fbs[i]);

        if (s_panel_fbs[i]) memset(s_panel_fbs[i], 0, kPhysWidth * kPhysHeight * sizeof(lv_color_t));
    }

    ESP_LOGI(TAG, "lv_init()");
    lv_init();
#if LV_USE_LODEPNG
    lv_lodepng_init();
#endif
    // Now that lv_init() has set up the default image draw-buf handlers, point
    // the decoded-image output buffer allocation at the PSRAM arena reserved
    // above. (lodepng's scratch is already routed via lodepng_malloc/free.)
    png_decode_arena_install_handlers();
    lv_tick_set_cb(lv_tick_cb);

    s_disp = lv_display_create(s_lvgl_w, s_lvgl_h);
    if (!s_disp) { ESP_LOGE(TAG, "lv_display_create failed"); return false; }
    lv_display_set_flush_cb(s_disp, flush_cb);
    lv_display_set_buffers(s_disp, lvgl_buf_a, lvgl_buf_b, buf_sz, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);

    if (touch_ok) {
        s_indev = lv_indev_create();
        lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(s_indev, touch_read_cb);
        lv_indev_set_display(s_indev, s_disp);
    }

    s_lvgl_mux = xSemaphoreCreateRecursiveMutex();
    if (!s_lvgl_mux) {
        ESP_LOGE(TAG, "failed to create LVGL mutex");
        return false;
    }

    psram_alloc_guard_arm();

    ESP_LOGI(TAG, "display_init complete (LVGL 9, %s %dx%d)",
             s_landscape ? "landscape" : "portrait", s_lvgl_w, s_lvgl_h);
    return true;
}

lv_display_t* lv_display() { return s_disp; }
lv_indev_t*   lv_indev()   { return s_indev; }

bool lvgl_lock(int timeout_ms)
{
    if (!s_lvgl_mux) return true;
    TickType_t t = (timeout_ms < 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTakeRecursive(s_lvgl_mux, t) == pdTRUE;
}
void lvgl_unlock() { if (s_lvgl_mux) xSemaphoreGiveRecursive(s_lvgl_mux); }

void display_start_lvgl_task()
{
    if (s_lvgl_task) return;
    if (!s_lvgl_mux || !s_disp) {
        ESP_LOGE(TAG, "display_start_lvgl_task called before display_init");
        return;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(
        lvgl_task,
        "lvgl",
        kLvglTaskStackBytes,
        nullptr,
        kLvglTaskPriority,
        &s_lvgl_task,
        kLvglTaskCore
    );
    if (ok != pdPASS) {
        s_lvgl_task = nullptr;
        ESP_LOGE(TAG, "failed to start LVGL service task");
    }
}

}
