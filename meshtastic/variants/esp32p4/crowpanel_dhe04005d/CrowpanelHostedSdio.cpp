#if defined(CROWPANEL_DHE04005D) && defined(ARCH_ESP32P4)

#include "board_config.h"

#include <esp_err.h>
#include <esp_event.h>
#include <esp_heap_caps.h>
#include <esp_hosted_event.h>
#include <esp_hosted_transport_config.h>
#include <esp_log.h>
#include <esp_private/sdmmc_common.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

extern "C" struct esp_hosted_sdio_config __real_esp_hosted_get_default_sdio_config(void);
extern "C" struct esp_hosted_sdio_config __real_esp_hosted_get_default_sdio_iomux_config(void);
extern "C" esp_err_t __real_sdmmc_io_rw_extended(sdmmc_card_t *card, int function, uint32_t reg, int arg, void *data,
                                                 size_t size);
extern "C" esp_err_t __real_sdmmc_send_cmd(sdmmc_card_t *card, sdmmc_command_t *cmd);

static portMUX_TYPE s_hostedRecoveryLock = portMUX_INITIALIZER_UNLOCKED;
static esp_event_handler_instance_t s_hostedEventInstance = nullptr;
static uint32_t s_sdioErrorWindowStartMs = 0;
static uint8_t s_sdioErrorCount = 0;
static bool s_hostedEventRegistered = false;
static bool s_hostedFailureWatchdogActive = false;
static bool s_hostedRestartScheduled = false;

static constexpr uint32_t kSdioErrorWindowMs = 30000;
// Map tile reads can produce a transient hosted-SDIO timeout on this board even
// when the link recovers. Restart only after repeated errors in the window.
static constexpr uint8_t kSdioErrorThreshold = 5;

static void crowpanelHostedRestartTask(void *arg)
{
    const char *reason = static_cast<const char *>(arg);
    ESP_LOGE("crowpanel", "ESP-Hosted WiFi link failed (%s); restarting in 2s", reason);
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
}

static void crowpanelHostedScheduleRestart(const char *reason)
{
    bool shouldCreateTask = false;
    portENTER_CRITICAL(&s_hostedRecoveryLock);
    if (!s_hostedRestartScheduled) {
        s_hostedRestartScheduled = true;
        shouldCreateTask = true;
    }
    portEXIT_CRITICAL(&s_hostedRecoveryLock);

    if (shouldCreateTask) {
        xTaskCreate(crowpanelHostedRestartTask, "hostedRestart", 3072, const_cast<char *>(reason), 5, nullptr);
    }
}

static void crowpanelHostedNoteSdioError(esp_err_t err)
{
    if (!s_hostedFailureWatchdogActive) {
        return;
    }

    const uint32_t nowMs = (uint32_t)(esp_timer_get_time() / 1000ULL);
    bool shouldRestart = false;

    portENTER_CRITICAL(&s_hostedRecoveryLock);
    if (s_sdioErrorWindowStartMs == 0 || (nowMs - s_sdioErrorWindowStartMs) > kSdioErrorWindowMs) {
        s_sdioErrorWindowStartMs = nowMs;
        s_sdioErrorCount = 0;
    }

    if (s_sdioErrorCount < UINT8_MAX) {
        s_sdioErrorCount++;
    }
    shouldRestart = s_sdioErrorCount == kSdioErrorThreshold;
    portEXIT_CRITICAL(&s_hostedRecoveryLock);

    if (shouldRestart) {
        ESP_LOGE("crowpanel",
                 "ESP-Hosted SDIO err=%s; scheduling WiFi-link recovery "
                 "(internal8 free=%u largest=%u dma free=%u largest=%u)",
                 esp_err_to_name(err),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
        crowpanelHostedScheduleRestart("repeated SDIO CRC/timeout errors");
    }
}

static void crowpanelHostedEventHandler(void *arg, esp_event_base_t eventBase, int32_t eventId, void *eventData)
{
    (void)arg;
    (void)eventData;

    if (eventBase == ESP_HOSTED_EVENT && eventId == ESP_HOSTED_EVENT_TRANSPORT_FAILURE) {
        crowpanelHostedScheduleRestart("ESP-Hosted transport failure event");
    }
}

static struct esp_hosted_sdio_config crowpanelHostedSdioConfig(struct esp_hosted_sdio_config config)
{
    config.bus_width = WIFI_HOSTED_SDIO_BUS_WIDTH;
    config.clock_freq_khz = WIFI_HOSTED_SDIO_CLOCK_KHZ;
    config.pin_clk.pin = WIFI_HOSTED_SDIO_PIN_CLK;
    config.pin_cmd.pin = WIFI_HOSTED_SDIO_PIN_CMD;
    config.pin_d0.pin = WIFI_HOSTED_SDIO_PIN_D0;
    config.pin_d1.pin = WIFI_HOSTED_SDIO_PIN_D1;
    config.pin_d2.pin = WIFI_HOSTED_SDIO_PIN_D2;
    config.pin_d3.pin = WIFI_HOSTED_SDIO_PIN_D3;
    config.pin_reset.pin = WIFI_HOSTED_SDIO_PIN_RESET;
    ESP_LOGI("crowpanel", "ESP-Hosted SDIO forced to %u-bit, %u kHz",
             (unsigned)config.bus_width, (unsigned)config.clock_freq_khz);
    return config;
}

extern "C" struct esp_hosted_sdio_config __wrap_esp_hosted_get_default_sdio_config(void)
{
    return crowpanelHostedSdioConfig(__real_esp_hosted_get_default_sdio_config());
}

extern "C" struct esp_hosted_sdio_config __wrap_esp_hosted_get_default_sdio_iomux_config(void)
{
    return crowpanelHostedSdioConfig(__real_esp_hosted_get_default_sdio_iomux_config());
}

extern "C" esp_err_t __wrap_sdmmc_io_rw_extended(sdmmc_card_t *card, int function, uint32_t reg, int arg, void *data,
                                                 size_t size)
{
    esp_err_t err = __real_sdmmc_io_rw_extended(card, function, reg, arg, data, size);
    if (err == ESP_ERR_INVALID_CRC || err == ESP_ERR_TIMEOUT) {
        crowpanelHostedNoteSdioError(err);
    } else if (err == ESP_OK) {
        portENTER_CRITICAL(&s_hostedRecoveryLock);
        s_sdioErrorCount = 0;
        s_sdioErrorWindowStartMs = 0;
        portEXIT_CRITICAL(&s_hostedRecoveryLock);
    }
    return err;
}

extern "C" esp_err_t __wrap_sdmmc_send_cmd(sdmmc_card_t *card, sdmmc_command_t *cmd)
{
    esp_err_t err = __real_sdmmc_send_cmd(card, cmd);
    if (err == ESP_ERR_INVALID_CRC || err == ESP_ERR_TIMEOUT) {
        crowpanelHostedNoteSdioError(err);
    }
    return err;
}

extern "C" void crowpanelHostedRegisterFailureHandler(void)
{
    s_hostedFailureWatchdogActive = true;

    if (s_hostedEventRegistered) {
        return;
    }

    esp_err_t err = esp_event_handler_instance_register(ESP_HOSTED_EVENT, ESP_EVENT_ANY_ID, crowpanelHostedEventHandler,
                                                       nullptr, &s_hostedEventInstance);
    if (err == ESP_OK) {
        s_hostedEventRegistered = true;
        ESP_LOGI("crowpanel", "ESP-Hosted transport failure watchdog registered");
    } else {
        ESP_LOGW("crowpanel", "ESP-Hosted watchdog registration failed: %s", esp_err_to_name(err));
    }
}

#endif
