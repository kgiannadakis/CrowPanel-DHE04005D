#pragma once
// ============================================================
// sd_storage.h — microSD mount for the CrowPanel DHE04005D
// ------------------------------------------------------------
// The board has a microSD slot wired to ESP32-P4's SDMMC slot 0
// (pins in board_config.h: SD_GPIO_MMC_CLK/CMD/D0). We use 1-bit
// SDMMC mode because that's what the Elecrow reference example
// ships — 4-bit would need extra routing we don't have.
//
// Mount point is "/sdcard" so every user-facing path in future
// features (maps, emoji pngs, music, etc.) can use standard POSIX
// `fopen("/sdcard/...")`. No card present → sd_init() logs a
// warning and returns false; the firmware continues to boot so
// everything that doesn't need SD keeps working.
// ============================================================

#include <stdint.h>
#include <stddef.h>

// Mount path. Stable — code elsewhere builds paths like
// "/sdcard/tiles/12/2054/1356.jpg" off this constant.
#define SD_MOUNT_POINT "/sdcard"

// Mount the SD card. Safe to call when no card is present — returns
// false and logs a warning, doesn't abort. Idempotent: calling a
// second time after a successful mount is a no-op and returns true.
bool sd_init();

// True if sd_init() has successfully mounted a card.
bool sd_is_mounted();

// Card capacity in bytes. 0 if not mounted.
uint64_t sd_total_bytes();

// Best-effort free-bytes estimate. Walking the FAT to compute free
// space is slow on large cards, so callers should cache the result.
// Returns 0 if not mounted.
uint64_t sd_free_bytes();
