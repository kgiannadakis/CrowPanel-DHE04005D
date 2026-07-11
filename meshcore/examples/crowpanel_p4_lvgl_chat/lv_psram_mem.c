// lv_psram_mem.c — see lv_psram_mem.h for rationale.

#include "lv_psram_mem.h"

#include <esp_heap_caps.h>

#define LV_PSRAM_CAPS   (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define LV_FALLBACK_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)

void* lv_psram_malloc(size_t size) {
  return heap_caps_malloc_prefer(size, 2, LV_PSRAM_CAPS, LV_FALLBACK_CAPS);
}

void* lv_psram_realloc(void* ptr, size_t size) {
  return heap_caps_realloc_prefer(ptr, size, 2, LV_PSRAM_CAPS, LV_FALLBACK_CAPS);
}

void lv_psram_free(void* ptr) {
  heap_caps_free(ptr);
}
