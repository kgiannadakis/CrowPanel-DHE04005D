#pragma once
//
// Minimal GT911 capacitive touch driver for CrowPanel DHE04005D.
// Lives on the shared I2C1 bus (SCL=IO46, SDA=IO45). INT=IO42. Reset is
// owned by the STC8 coprocessor (STC8_GPIO_OUT_TP_RST), not by the P4.
//
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Call once from setup(), AFTER stc8 is up (we need it to toggle TP_RST).
bool gt911_init(uint16_t panel_w, uint16_t panel_h);

// Read the latest touch point. Returns true if a finger is currently down.
// When true, *x, *y are filled in panel coordinates.
bool gt911_read(uint16_t* x, uint16_t* y);

#ifdef __cplusplus
}
#endif
