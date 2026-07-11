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
extern "C" void crowpanel_lvgl_alloc_reserve(void);
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

// Bytes per pixel of the actual framebuffer / render format. The panel is
// RGB565 (cfg.bits_per_pixel = 16) and LVGL renders RGB565
// (lv_display_set_color_format(..., RGB565)). Do NOT use sizeof(lv_color_t) for
// pixel-byte math: in LVGL 9 lv_color_t is a 3-byte RGB888 struct, so using it
// as bytes/px makes every stride 1.5x too large. That corrupted the landscape
// direct-blit (portrait's PPA path derives its stride from the RGB565 color
// mode, so it was unaffected — which is why only landscape smeared).
static constexpr size_t kBytesPerPixel = 2;

static constexpr int kFrameBufferMax = 2;
static constexpr uint32_t kLvglTickMs = 2;
// 20 KB: the map's PNG-decode (lodepng) + LVGL SW-draw recursion is the deepest
// call chain in this task and overflowed the old 8704-byte stack under load
// (Stack protection fault in task "lvgl", SP below stack base). Internal RAM has
// ample headroom (~230 KB largest free block), so this is a cheap safety margin.
static constexpr int kLvglTaskStackBytes = 20480;
static constexpr UBaseType_t kLvglTaskPriority = 5;
static constexpr BaseType_t kLvglTaskCore = 0;
static constexpr uint32_t kLvglTaskSleepMaxMs = 6;
// RGB bounce buffer depth. Empirically settled: 8 rows underruns during
// PSRAM bandwidth spikes and shows as whole-screen horizontal shearing (the
// double-FB flip does NOT cover this failure mode — verified by A/B builds);
// 16 rows (~51 KB DMA internal) keeps the FIFO fed. The DMA budget for this
// is paid on the MQTT side: under memory pressure the broker connection is
// DISCONNECTED (not just paused — a paused socket accumulates lwIP pbufs in
// this same pool) and re-established when the pool recovers.
static constexpr int kRgbBounceRows = 16;

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
// Anti-tearing: the panel scans out of one framebuffer while we compose the
// next frame into the other, then flip. s_frame_sem is given from the RGB
// ISR each time a complete framebuffer finished going out to the LCD, so a
// flip is known to have taken effect before we touch the retired buffer.
static SemaphoreHandle_t      s_frame_sem = nullptr;
static int                    s_back_fb = 1;      // FB we blit into next (not displayed)
static uint8_t*               s_lvgl_fb = nullptr; // full-size logical canvas (LVGL DIRECT mode)

// ISR context (keep minimal): full frame finished scanning out. Must be in
// IRAM: the framework's RGB driver is built with LCD_RGB_ISR_IRAM_SAFE and
// rejects flash-resident callbacks (ESP_ERR_INVALID_ARG at registration).
static bool IRAM_ATTR on_frame_done_cb(esp_lcd_panel_handle_t, const esp_lcd_rgb_panel_event_data_t*, void*)
{
    BaseType_t woken = pdFALSE;
    if (s_frame_sem) xSemaphoreGiveFromISR(s_frame_sem, &woken);
    return woken == pdTRUE;
}

// Dirty-rect history for double buffering: after a flip, the new back FB is
// one frame behind, so it must additionally receive the rects that changed in
// the PREVIOUS cycle. Blitting whole frames instead saturates PSRAM bandwidth
// (starves ESP-Hosted RX until the SDIO driver asserts) — do not do that.
static constexpr int kMaxDirtyRects = 12;
static lv_area_t s_prev_rects[kMaxDirtyRects];
static int       s_prev_rect_count = -1; // -1 = full-frame catch-up needed
static lv_area_t s_cur_rects[kMaxDirtyRects];
static int       s_cur_rect_count = 0;   // -1 = overflowed, full-frame next time

// Blit one logical-coordinate rect from the LVGL canvas into a panel FB.
static void blit_rect(lv_color_t* fb, const lv_area_t* area)
{
    if (s_landscape) {
        const int w = area->x2 - area->x1 + 1;
        for (int y = area->y1; y <= area->y2; ++y) {
            memcpy((uint8_t*)fb + ((size_t)y * kPhysWidth + area->x1) * kBytesPerPixel,
                   s_lvgl_fb + ((size_t)y * kLandWidth + area->x1) * kBytesPerPixel,
                   (size_t)w * kBytesPerPixel);
        }
        return;
    }

    if (!s_ppa_srm) return;
    ppa_srm_oper_config_t oper = {};
    oper.in.buffer          = s_lvgl_fb;
    oper.in.pic_w           = kPortWidth;
    oper.in.pic_h           = kPortHeight;
    oper.in.block_w         = area->x2 - area->x1 + 1;
    oper.in.block_h         = area->y2 - area->y1 + 1;
    oper.in.block_offset_x  = area->x1;
    oper.in.block_offset_y  = area->y1;
    oper.in.srm_cm          = PPA_SRM_COLOR_MODE_RGB565;

    oper.out.buffer         = fb;
    oper.out.buffer_size    = (uint32_t)kPhysWidth * (uint32_t)kPhysHeight * kBytesPerPixel;
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
    if (err != ESP_OK) ESP_LOGE(TAG, "blit PPA failed: %d", (int)err);
}

static void blit_full(lv_color_t* fb)
{
    lv_area_t whole = {0, 0, (int32_t)(s_lvgl_w - 1), (int32_t)(s_lvgl_h - 1)};
    blit_rect(fb, &whole);
}

// True while a flip has been requested but the retired framebuffer may still
// be scanning out. Resolved lazily at the START of the next flush so LVGL can
// render the next frame in parallel with the flip instead of blocking.
static bool s_flip_pending = false;

static void wait_flip_complete()
{
    if (!s_flip_pending)
        return;
    if (s_frame_sem)
        xSemaphoreTake(s_frame_sem, pdMS_TO_TICKS(50));
    s_flip_pending = false;
}

static void flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px)
{
    (void)px;
    lv_color_t* fb = s_panel_fbs[s_back_fb];
    if (!fb || !s_lvgl_fb) {
        lv_display_flush_ready(disp);
        return;
    }

    // Before touching the back FB, make sure the panel finished retiring it.
    wait_flip_complete();

    // DIRECT mode calls flush per dirty area; blit each into the back FB and
    // remember it for next cycle's catch-up.
    blit_rect(fb, area);
    if (s_cur_rect_count >= 0) {
        if (s_cur_rect_count < kMaxDirtyRects)
            s_cur_rects[s_cur_rect_count++] = *area;
        else
            s_cur_rect_count = -1; // too many: full-frame catch-up next cycle
    }

    if (lv_display_flush_is_last(disp)) {
        // Catch the back FB up with everything that changed last cycle
        // (it was off-screen then and missed those blits).
        if (s_prev_rect_count < 0) {
            blit_full(fb);
        } else {
            for (int i = 0; i < s_prev_rect_count; ++i)
                blit_rect(fb, &s_prev_rects[i]);
        }

        // Flip: passing a framebuffer pointer to draw_bitmap makes the RGB
        // driver switch scanout to it (no copy). Do NOT wait here — mark the
        // flip pending and return so LVGL renders the next frame while the
        // panel switches; wait_flip_complete() resolves it before the next
        // blit into the retired buffer.
        if (s_frame_sem) xSemaphoreTake(s_frame_sem, 0); // drain stale signal
        esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, 0, 0, kPhysWidth, kPhysHeight, fb);
        if (err != ESP_OK) ESP_LOGE(TAG, "flip draw_bitmap failed: %d", (int)err);
        s_flip_pending = true;
        s_back_fb ^= 1;

        // Rotate dirty history
        s_prev_rect_count = s_cur_rect_count;
        if (s_cur_rect_count > 0)
            memcpy(s_prev_rects, s_cur_rects, (size_t)s_cur_rect_count * sizeof(lv_area_t));
        s_cur_rect_count = 0;
    }

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

    // Anti-tearing pipeline: LVGL renders (DIRECT mode) into one full-size
    // logical canvas in PSRAM; each finished frame is blitted whole into the
    // off-screen panel framebuffer and flipped at frame boundaries. This
    // replaces the old partial row-strip buffers, whose mid-scanout writes
    // into the live framebuffer caused heavy tearing.
    const size_t buf_sz = (size_t)s_lvgl_w * (size_t)s_lvgl_h * kBytesPerPixel;
    s_lvgl_fb = (uint8_t*)heap_caps_aligned_alloc(128, buf_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_lvgl_fb) {
        ESP_LOGE(TAG, "lvgl canvas alloc failed (%u bytes)", (unsigned)buf_sz);
        return false;
    }
    memset(s_lvgl_fb, 0, buf_sz);

    ESP_LOGI(TAG, "LVGL full canvas %dx%d (%u KB, PSRAM)", s_lvgl_w, s_lvgl_h, (unsigned)(buf_sz / 1024));

    // Reserve the image-decode PSRAM arena now, while the system PSRAM heap is
    // still clean. esp_lcd_panel_init() (below) corrupts that heap's free-list,
    // after which runtime PSRAM allocations assert. The arena's private
    // multi_heap is unaffected because its memory is reserved here, up-front.
    png_decode_arena_reserve();
    // LVGL's custom allocator SHARES the arena's multi_heap (unified single heap
    // for LVGL structs + decoded image buffers, so a buffer can't be freed into
    // the wrong heap). This call is a no-op once the arena is reserved; it only
    // reserves a private fallback block if the arena reservation above failed.
    // lv_mem_init() (from lv_init(), post-framebuffer) picks up the shared heap.
    crowpanel_lvgl_alloc_reserve();

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

    // Two PSRAM framebuffers: scanout reads one while the next frame is
    // composed into the other, flipped at frame boundaries (anti-tearing).
    // Both are allocated inside esp_lcd_new_rgb_panel(), i.e. before the
    // PSRAM heap guard arms, consistent with the heap discipline here.
    cfg.num_fbs               = 2;
    cfg.bounce_buffer_size_px = kPhysWidth * kRgbBounceRows;
    cfg.dma_burst_size        = 64;
    cfg.flags.fb_in_psram     = 1;

    esp_err_t err = esp_lcd_new_rgb_panel(&cfg, &s_panel);
    if (err != ESP_OK) { ESP_LOGE(TAG, "rgb_panel: %d", err); return false; }
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);

    s_frame_sem = xSemaphoreCreateBinary();
    if (s_frame_sem) {
        esp_lcd_rgb_panel_event_callbacks_t cbs = {};
        cbs.on_frame_buf_complete = on_frame_done_cb;
        esp_err_t cb_err = esp_lcd_rgb_panel_register_event_callbacks(s_panel, &cbs, nullptr);
        if (cb_err != ESP_OK) {
            ESP_LOGW(TAG, "register frame callbacks failed: %d (flips fall back to timeout)", (int)cb_err);
            vSemaphoreDelete(s_frame_sem);
            s_frame_sem = nullptr;
        }
    }

    void* raw_fbs[kFrameBufferMax] = {};
    err = esp_lcd_rgb_panel_get_frame_buffer(s_panel, 2, &raw_fbs[0], &raw_fbs[1]);
    if (err != ESP_OK) { ESP_LOGE(TAG, "get_fb: %d", err); return false; }
    for (size_t i = 0; i < kFrameBufferMax; ++i) {
        s_panel_fbs[i] = static_cast<lv_color_t*>(raw_fbs[i]);

        if (s_panel_fbs[i]) memset(s_panel_fbs[i], 0, kPhysWidth * kPhysHeight * kBytesPerPixel);
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
    // DIRECT mode: LVGL renders dirty areas straight into the persistent
    // full-size canvas; flush_cb publishes the whole frame with a flip.
    lv_display_set_buffers(s_disp, s_lvgl_fb, nullptr, buf_sz, LV_DISPLAY_RENDER_MODE_DIRECT);
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
