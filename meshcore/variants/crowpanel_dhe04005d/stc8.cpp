#include "stc8.h"
#include "i2c_bus.h"
#include <esp_log.h>

static const char* TAG = "stc8";

// STC8 register map (from Elecrow's Lesson09 bsp_stc8h1kxx.h)
static constexpr uint8_t STC8_ADDR          = 0x2F;
static constexpr uint8_t REG_SET_GPIO_BASE  = 0x18;
static constexpr uint8_t REG_SET_PWM_BASE   = 0x20;

static constexpr uint32_t I2C_FREQ_HZ = 400000;
static constexpr int      I2C_TIMEOUT_MS = 100;

static i2c_master_dev_handle_t s_dev = NULL;

static bool stc8_ensure_device() {
  if (s_dev != NULL) return true;

  i2c_master_bus_handle_t bus = i2c1_bus_handle();
  if (!bus) return false;

  i2c_device_config_t dev_cfg = {};
  dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  dev_cfg.device_address  = STC8_ADDR;
  dev_cfg.scl_speed_hz    = I2C_FREQ_HZ;

  esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "i2c_master_bus_add_device failed: %d", (int)err);
    s_dev = NULL;
    return false;
  }
  return true;
}

static esp_err_t stc8_write_reg(uint8_t reg, uint8_t value) {
  if (!stc8_ensure_device()) return ESP_FAIL;
  uint8_t buf[2] = { reg, value };
  return i2c_master_transmit(s_dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

esp_err_t stc8_set_pwm_duty(int pwm_num, uint8_t duty) {
  if (pwm_num < 0 || pwm_num >= STC8_PWM_MAX) {
    ESP_LOGE(TAG, "invalid pwm channel %d", pwm_num);
    return ESP_ERR_INVALID_ARG;
  }
  if (duty > 100) duty = 100;
  esp_err_t err = stc8_write_reg(REG_SET_PWM_BASE + pwm_num, duty);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "set_pwm_duty(%d,%u) failed: %d", pwm_num, duty, (int)err);
  }
  return err;
}

esp_err_t stc8_gpio_set_level(int gpio_num, uint8_t level) {
  if (gpio_num < 0 || gpio_num >= STC8_GPIO_OUT_MAX) {
    ESP_LOGE(TAG, "invalid gpio out %d", gpio_num);
    return ESP_ERR_INVALID_ARG;
  }
  esp_err_t err = stc8_write_reg(REG_SET_GPIO_BASE + gpio_num, level ? 1 : 0);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "gpio_set_level(%d,%u) failed: %d", gpio_num, level, (int)err);
  }
  return err;
}
