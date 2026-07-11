#pragma once

#include <stdint.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  STC8_PWM_LCD_BL_EN = 0,
  STC8_PWM_MAX,
};

enum {
  STC8_GPIO_OUT_TP_RST = 0,
  STC8_GPIO_OUT_CSI_RST,
  STC8_GPIO_OUT_AUDIO_SD,
  STC8_GPIO_OUT_LCD_BL_POWER,
  STC8_GPIO_OUT_MAX,
};

// Battery charge state reported by the STC8 (EM_BAT_CHARGE_STATE in Elecrow's
// factory bsp_stc8h1kxx driver).
enum {
  STC8_BAT_IDLE = 0,
  STC8_BAT_CHARGING,      // charging
  STC8_BAT_FULLY_CHARGED, // full
  STC8_BAT_NO_CHARGE,     // not charging (no charger present)
  STC8_BAT_ERROR,
};

esp_err_t stc8_set_pwm_duty(int pwm_num, uint8_t duty);

esp_err_t stc8_gpio_set_level(int gpio_num, uint8_t level);

// Read battery info from the STC8 (I2C 0x2F, register block starting at 0x00).
// Any non-null out-param is filled: level_pct (0-100), charge_state (STC8_BAT_*),
// millivolts (divider-corrected battery voltage). Returns ESP_OK on success.
esp_err_t stc8_read_battery(uint8_t *level_pct, uint8_t *charge_state, uint16_t *millivolts);

#ifdef __cplusplus
}
#endif
