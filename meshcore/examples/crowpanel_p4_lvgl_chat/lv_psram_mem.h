// lv_psram_mem.h — PSRAM-first allocator for LVGL's internal heap.
//
// Wired in via LV_MEM_CUSTOM_* in lv_conf.h. With the stock malloc route,
// esp-idf serves every allocation <= CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL
// (16 KB) from internal SRAM first, so LVGL's thousands of small persistent
// allocations (widgets, styles, label texts, timers, draw temp buffers)
// drain the ~500 KB internal pool that WiFi/mbedtls/ESP-Hosted also need —
// observed as "Translate: low mem skip" with <13 KB internal free.
//
// Nothing allocated through lv_mem is a DMA target in this app: the LVGL
// draw buffers and the RGB panel framebuffers are allocated separately in
// display.cpp with explicit heap_caps, and the PPA only touches those. So
// LVGL's heap can live in PSRAM wholesale, with internal SRAM as fallback
// only when PSRAM is exhausted.

#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void* lv_psram_malloc(size_t size);
void* lv_psram_realloc(void* ptr, size_t size);
void  lv_psram_free(void* ptr);

#ifdef __cplusplus
}
#endif
