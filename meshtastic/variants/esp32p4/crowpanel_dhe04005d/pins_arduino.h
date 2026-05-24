#pragma once

#include <stdint.h>

#define LED_BUILTIN -1

#define PIN_WIRE_SDA 45
#define PIN_WIRE_SCL 46
static const uint8_t SDA = PIN_WIRE_SDA;
static const uint8_t SCL = PIN_WIRE_SCL;

#include "board_config.h"
static const uint8_t SS   = LORA_CS;
static const uint8_t MOSI = PIN_SPI_MOSI;
static const uint8_t MISO = PIN_SPI_MISO;
static const uint8_t SCK  = PIN_SPI_SCK;
