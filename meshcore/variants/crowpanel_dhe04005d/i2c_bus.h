#pragma once
//
// Shared I2C master bus for the CrowPanel DHE04005D.
// Both the STC8 coprocessor (backlight/GPIO/battery) and the GT911 touch
// controller sit on I2C1 (SCL=IO46, SDA=IO45). Use i2c1_bus_handle() to
// get the master bus, then i2c_master_bus_add_device() for each slave.
//
#include <driver/i2c_master.h>

#ifdef __cplusplus
extern "C" {
#endif

// Lazy-initialised on first call. Returns NULL on failure.
i2c_master_bus_handle_t i2c1_bus_handle(void);

#ifdef __cplusplus
}
#endif
