/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "utils/esp_panel_utils_log.h"
#include "esp_panel_host_i2c.hpp"

// ----- arduino-esp32 3.x patch -----
// Original code uses i2c_driver_install / i2c_param_config / i2c_driver_delete
// (legacy IDF i2c.c API). Even with USE_TOUCH=0 this .cpp gets linked, and
// any reference to those legacy symbols pulls in libdriver.a(i2c.c.obj),
// whose .text.startup ctor `check_i2c_driver_conflict()` aborts at boot
// because the new i2c-NG driver is also linked. Stubbing the bodies here
// keeps the symbols out of our link line so the abort never fires.

namespace esp_panel::drivers {

HostI2C::~HostI2C()
{
    ESP_UTILS_LOG_TRACE_ENTER_WITH_THIS();

    if (isOverState(State::BEGIN)) {
        ESP_UTILS_LOGE("HostI2C destructor reached but legacy I2C driver is disabled in this build");
        setState(State::DEINIT);
    }

    ESP_UTILS_LOG_TRACE_EXIT_WITH_THIS();
}

bool HostI2C::begin()
{
    ESP_UTILS_LOG_TRACE_ENTER_WITH_THIS();
    ESP_UTILS_LOGE("HostI2C::begin() called but legacy I2C driver is disabled in this build "
                   "(arduino-esp32 3.x i2c-NG conflict). Wire up I2C with the new i2c_master_* API instead.");
    return false;
}

bool HostI2C::calibrateConfig(const i2c_config_t &config)
{
    if (memcmp(&config, &this->config, sizeof(i2c_config_t))) {
        ESP_UTILS_LOGI(
            "Original config: mode(%d), sda_io_num(%d), scl_io_num(%d), sda_pullup_en(%d), scl_pullup_en(%d),"
            "clk_speed(%d)", this->config.mode, this->config.sda_io_num, this->config.scl_io_num,
            this->config.sda_pullup_en, this->config.scl_pullup_en, static_cast<int>(this->config.master.clk_speed)
        );
        ESP_UTILS_LOGI(
            "New config: mode(%d), sda_io_num(%d), scl_io_num(%d), sda_pullup_en(%d), scl_pullup_en(%d), clk_speed(%d)",
            config.mode, config.sda_io_num, config.scl_io_num, config.sda_pullup_en, config.scl_pullup_en,
            static_cast<int>(config.master.clk_speed)
        );
        ESP_UTILS_CHECK_FALSE_RETURN(false, false, "Config mismatch");
    }

    return true;
}

} // namespace esp_panel::drivers
