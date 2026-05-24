#pragma once
//
// STC8 coprocessor driver — CrowPanel DHE04005D.
// The STC8H1KXX on this board owns the LCD backlight PWM, the touch reset
// pin, audio amp enable, camera reset, and reports battery state. It is
// addressed at I2C 0x2F on the shared I2C1 bus.
//
// Ported from Elecrow's Lesson09 bsp_stc8h1kxx.c to the new ESP-IDF
// i2c_master_bus driver.
//
#include <stdint.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

// PWM channels
enum {
  STC8_PWM_LCD_BL_EN = 0,   // LCD backlight
  STC8_PWM_MAX,
};

// GPIO outputs
enum {
  STC8_GPIO_OUT_TP_RST = 0,
  STC8_GPIO_OUT_CSI_RST,
  STC8_GPIO_OUT_AUDIO_SD,
  STC8_GPIO_OUT_LCD_BL_POWER,
  STC8_GPIO_OUT_MAX,
};

// duty: 0..100 (percent)
esp_err_t stc8_set_pwm_duty(int pwm_num, uint8_t duty);

// level: 0 or 1
esp_err_t stc8_gpio_set_level(int gpio_num, uint8_t level);

#ifdef __cplusplus
}
#endif
