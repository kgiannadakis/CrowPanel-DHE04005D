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

esp_err_t stc8_set_pwm_duty(int pwm_num, uint8_t duty);

esp_err_t stc8_gpio_set_level(int gpio_num, uint8_t level);

#ifdef __cplusplus
}
#endif
