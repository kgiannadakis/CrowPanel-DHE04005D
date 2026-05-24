#include "crowpanel_backlight.h"

#include <Arduino.h>
#include "i2c_bus.h"
#include "stc8.h"
#include <LittleFS.h>
#include <esp_partition.h>
#include <esp_log.h>
#include <WiFi.h>
#include <esp32-hal-hosted.h>
#include "../variants/esp32p4/crowpanel_dhe04005d/board_config.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

static constexpr uint8_t  BL_OFF = 0x05;
static constexpr uint8_t  BL_MAX = 0x10;
static constexpr uint32_t DEFAULT_TIMEOUT_SECS = 30;
static constexpr uint32_t SOFT_WAKE_MS = 1500;
static constexpr uint32_t STARTUP_GRACE_MS = 5000;
static constexpr uint32_t TASK_PERIOD_MS = 100;
#if CROWPANEL_RETURN_TO_BOOT_SELECTOR
static const char *TAG = "crowpanel";
#endif

static SemaphoreHandle_t s_wire_lock = nullptr;

static void _bl_cmd(uint8_t cmd) {
    if (s_wire_lock) xSemaphoreTake(s_wire_lock, portMAX_DELAY);
    uint8_t duty = 0;
    if (cmd > BL_OFF) {
        duty = (uint8_t)(((uint32_t)(cmd - BL_OFF) * 100U) / (BL_MAX - BL_OFF));
    }
    stc8_set_pwm_duty(STC8_PWM_LCD_BL_EN, duty);
    if (s_wire_lock) xSemaphoreGive(s_wire_lock);
}

static volatile uint32_t s_last_activity_ms = 0;
static volatile bool     s_wake_requested   = false;
static volatile bool     s_screen_on        = true;
static volatile uint32_t s_timeout_secs     = DEFAULT_TIMEOUT_SECS;

static void _bl_soft_wake() {
    const uint8_t start = BL_OFF + 1;
    const uint8_t end   = BL_MAX;
    const uint8_t steps = end - start;

    for (uint8_t i = 0; i <= steps; i++) {
        _bl_cmd(start + i);
        if (i < steps) {
            uint32_t dt = SOFT_WAKE_MS * (2 * i + 1) / (steps * steps);
            delay(dt);
        }
    }
}

extern "C" void backlight_notify_activity(void) {
    s_last_activity_ms = millis();
    if (!s_screen_on) s_wake_requested = true;
}

extern "C" bool backlight_is_screen_on(void) {
    return s_screen_on;
}

extern "C" void backlight_set_timeout_secs(uint32_t secs) {

    if (secs == 0) secs = DEFAULT_TIMEOUT_SECS;
    s_timeout_secs = secs;
}

extern "C" void backlight_i2c_lock(void) {
    if (s_wire_lock) xSemaphoreTake(s_wire_lock, portMAX_DELAY);
}
extern "C" void backlight_i2c_unlock(void) {
    if (s_wire_lock) xSemaphoreGive(s_wire_lock);
}

static volatile bool s_fb_gate_released = false;

extern "C" void crowpanel_p4_fb_gate_release(void) {
    s_fb_gate_released = true;
}

extern "C" void crowpanel_p4_fb_gate_wait(uint32_t timeout_ms) {
    const uint32_t start = millis();
    while (!s_fb_gate_released) {
        if ((millis() - start) >= timeout_ms) {
            ESP_LOGW("crowpanel",
                     "FB gate timeout (%u ms) - proceeding; network may be unsafe",
                     (unsigned)timeout_ms);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_LOGI("crowpanel", "FB gate released after %u ms",
             (unsigned)(millis() - start));
}

static void backlight_task(void *) {

    vTaskDelay(pdMS_TO_TICKS(STARTUP_GRACE_MS));
    s_last_activity_ms = millis();

    while (true) {

        if (s_wake_requested) {
            s_wake_requested = false;
            if (!s_screen_on) {
                _bl_soft_wake();
                s_screen_on = true;
            }
            s_last_activity_ms = millis();
        }

        uint32_t to = s_timeout_secs;
        if (to != UINT32_MAX && s_screen_on) {
            uint32_t elapsed = millis() - s_last_activity_ms;
            if (elapsed >= to * 1000U) {
                _bl_cmd(BL_OFF);
                s_screen_on = false;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(TASK_PERIOD_MS));
    }
}

extern "C" void initVariant() {

#if CROWPANEL_RETURN_TO_BOOT_SELECTOR
    const esp_partition_t *factory = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);
    const esp_partition_t *otadata = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, nullptr);
    if (factory && otadata) {
        esp_err_t err = esp_partition_erase_range(otadata, 0, otadata->size);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "cleared otadata; factory boot selector will run on next reboot");
        } else {
            ESP_LOGW(TAG, "failed to clear otadata (%d)", (int)err);
        }
    } else {
        ESP_LOGW(TAG, "boot-selector return requested but factory/otadata partition is missing");
    }
#endif

    LittleFS.begin(true, "/littlefs", 10, "mtdata");

    // Prewarm ESP-Hosted + WiFi before RGB/LVGL allocations land in PSRAM.
    // On this board that avoids scan-time TLSF asserts in hosted/lwip alloc paths.
    WiFi.setPins(WIFI_HOSTED_SDIO_PIN_CLK,
                 WIFI_HOSTED_SDIO_PIN_CMD,
                 WIFI_HOSTED_SDIO_PIN_D0,
                 WIFI_HOSTED_SDIO_PIN_D1,
                 WIFI_HOSTED_SDIO_PIN_D2,
                 WIFI_HOSTED_SDIO_PIN_D3,
                 WIFI_HOSTED_SDIO_PIN_RESET);
    if (!hostedInitWiFi()) {
        ESP_LOGW("crowpanel", "ESP-Hosted pre-init failed; WiFi may be unstable");
    } else {
        WiFi.useStaticBuffers(true);
        WiFi.mode(WIFI_STA);
        WiFi.disconnect();
        ESP_LOGI("crowpanel", "ESP-Hosted prewarmed (STA mode, disconnected)");
    }

    i2c1_bus_handle();
    delay(50);

    s_wire_lock = xSemaphoreCreateMutex();

    _bl_soft_wake();
    s_screen_on = true;
    s_last_activity_ms = millis();

    xTaskCreatePinnedToCore(
        backlight_task,
        "blTask",
        1024,
        nullptr,
        1,
        nullptr,
        1
    );
}
