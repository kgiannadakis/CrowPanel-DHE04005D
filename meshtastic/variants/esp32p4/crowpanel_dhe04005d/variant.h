#pragma once

#include "board_config.h"

#define LORA_TYPE   "sx1262"
#define HAS_RADIO   1

#define I2C_SDA     I2C_GPIO_SDA
#define I2C_SCL     I2C_GPIO_SCL

#define HAS_TOUCHSCREEN 1
#define TOUCH_I2C_PORT  0
#define TOUCH_SLAVE_ADDRESS 0x5D

#define TFT_WIDTH   H_size
#define TFT_HEIGHT  V_size

#define HAS_WIFI 1

#define GPS_DEFAULT_NOT_PRESENT 1
