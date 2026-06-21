/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

// Define the log tag for the current library, should be declared before `esp_lib_utils.hpp`
#define ESP_UTILS_LOG_TAG "Expander"
#include "esp_lib_utils.h"
#include "esp_utils_helpers.h"

#if defined(CONFIG_IDF_TARGET_ESP32P4) && defined(CROWPANEL_DHE04209D)
#include <stddef.h>
#include <stdint.h>

#include "driver/i2c.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

static inline esp_err_t esp_expander_legacy_i2c_not_supported(const char *operation)
{
    ESP_UTILS_LOGE("%s is disabled on ESP32-P4 to avoid old/new I2C driver conflicts", operation);
    return ESP_ERR_NOT_SUPPORTED;
}

static inline esp_err_t esp_expander_i2c_master_write_to_device(
    i2c_port_t i2c_num, uint8_t device_address, const uint8_t *write_buffer, size_t write_size,
    TickType_t ticks_to_wait
)
{
    (void)i2c_num;
    (void)device_address;
    (void)write_buffer;
    (void)write_size;
    (void)ticks_to_wait;
    return esp_expander_legacy_i2c_not_supported("i2c_master_write_to_device");
}

static inline esp_err_t esp_expander_i2c_master_read_from_device(
    i2c_port_t i2c_num, uint8_t device_address, uint8_t *read_buffer, size_t read_size,
    TickType_t ticks_to_wait
)
{
    (void)i2c_num;
    (void)device_address;
    (void)read_buffer;
    (void)read_size;
    (void)ticks_to_wait;
    return esp_expander_legacy_i2c_not_supported("i2c_master_read_from_device");
}

static inline esp_err_t esp_expander_i2c_master_write_read_device(
    i2c_port_t i2c_num, uint8_t device_address, const uint8_t *write_buffer, size_t write_size,
    uint8_t *read_buffer, size_t read_size, TickType_t ticks_to_wait
)
{
    (void)i2c_num;
    (void)device_address;
    (void)write_buffer;
    (void)write_size;
    (void)read_buffer;
    (void)read_size;
    (void)ticks_to_wait;
    return esp_expander_legacy_i2c_not_supported("i2c_master_write_read_device");
}

#define i2c_master_write_to_device esp_expander_i2c_master_write_to_device
#define i2c_master_read_from_device esp_expander_i2c_master_read_from_device
#define i2c_master_write_read_device esp_expander_i2c_master_write_read_device
#endif
