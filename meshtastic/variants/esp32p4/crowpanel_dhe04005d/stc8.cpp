#include "stc8.h"
#include "i2c_bus.h"
#include <esp_log.h>

static const char* TAG = "stc8";

static constexpr uint8_t STC8_ADDR          = 0x2F;
static constexpr uint8_t REG_BATTERY_BASE   = 0x00;
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

static esp_err_t stc8_read_reg(uint8_t reg, uint8_t* value) {
  if (!stc8_ensure_device()) return ESP_FAIL;
  return i2c_master_transmit_receive(s_dev, &reg, 1, value, 1, I2C_TIMEOUT_MS);
}

esp_err_t stc8_read_battery(uint8_t* level_pct, uint8_t* charge_state, uint16_t* millivolts) {
  // Battery_info_t on the STC8, consecutive registers from 0x00 (matches
  // Elecrow's factory bsp_stc8h1kxx driver):
  //   [0..3]  adc_voltage   u32, mV
  //   [4..7]  bat_voltage   u32, mV (after the divider — the real battery mV)
  //   [8]     bat_level     u8,  %
  //   [9]     bat_state     u8   (STC8_BAT_*)
  //   [10]    led_state     u8
  // Read one register per transaction (the STC8 does not auto-increment its
  // register pointer within a single I2C read), and only the fields requested,
  // so a status-bar poll (level + state) is just 2 quick transactions rather
  // than 11 — keeps the caller's task from stalling.
  esp_err_t err;
  if (millivolts) {
    uint8_t b[4];
    for (int i = 0; i < 4; i++) {
      err = stc8_read_reg(REG_BATTERY_BASE + 4 + (uint8_t)i, &b[i]);
      if (err != ESP_OK) return err;
    }
    uint32_t mv = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
                  ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    *millivolts = (mv > 0xFFFF) ? 0xFFFF : (uint16_t)mv;
  }
  if (level_pct) {
    err = stc8_read_reg(REG_BATTERY_BASE + 8, level_pct);
    if (err != ESP_OK) return err;
  }
  if (charge_state) {
    err = stc8_read_reg(REG_BATTERY_BASE + 9, charge_state);
    if (err != ESP_OK) return err;
  }
  return ESP_OK;
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
