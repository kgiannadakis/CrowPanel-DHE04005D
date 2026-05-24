#pragma once
// ============================================================
// ds1307.h — DS1307 RTC over the shared I2C1 bus.
//
// Battery-backed timekeeping so the clock survives reboots + power
// loss. Sits on the same I2C master bus as the GT911 touch controller
// (SCL=IO46, SDA=IO45) at 7-bit addr 0x68, 100 kHz max.
// ============================================================

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Probes the chip once. Returns true on success. Safe to call multiple
// times — subsequent calls are idempotent and return the cached result.
bool ds1307_begin(void);

// True iff ds1307_begin() previously found a responsive chip at 0x68.
bool ds1307_present(void);

// Reads the DS1307 time as a Unix epoch (UTC seconds since 1970).
// Returns false if the chip isn't present or the clock-halt (CH) bit
// is set — DS1307 asserts CH on first power-up with a dead/fresh
// battery, meaning no valid time is stored yet.
bool ds1307_read_unix(uint32_t* out_utc);

// Writes a Unix epoch (UTC) to the chip and clears the CH bit so the
// oscillator runs. Returns false on I2C error or if the chip isn't
// present.
bool ds1307_write_unix(uint32_t utc);

#ifdef __cplusplus
}
#endif
