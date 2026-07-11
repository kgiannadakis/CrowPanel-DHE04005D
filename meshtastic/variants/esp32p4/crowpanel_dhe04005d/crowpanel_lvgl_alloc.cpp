// crowpanel_lvgl_alloc — LVGL custom allocator backed by a PSRAM multi_heap.
//
// WHY THIS EXISTS
// ---------------
// Two hard constraints on this board conflict under LVGL's stock allocators:
//   1. LVGL's allocations must live in PSRAM, not internal/DMA RAM. With
//      LV_STDLIB_CLIB (plain malloc) and this board's heap discipline
//      (heap_caps_malloc_extmem_enable(SIZE_MAX) reserves PSRAM for the
//      framebuffers), every lv_malloc lands in internal RAM and starves
//      ESP-Hosted's SDIO/DMA pool -> driver asserts.
//   2. LVGL's LV_STDLIB_BUILTIN allocator (its bundled lv_tlsf) hits an
//      infinite loop in lv_tlsf_free during the map's image decode/free
//      pattern, hanging the render task — even with a completely healthy,
//      un-fragmented heap (proven: pool monitor showed 6 MB free / 1% frag
//      right before the hang; reverting to CLIB made the map render). It is a
//      latent lv_tlsf defect, not a capacity problem.
//
// The fix: LV_USE_STDLIB_MALLOC = LV_STDLIB_CUSTOM, with lv_*_core routed to a
// FreeRTOS multi_heap placed over a PSRAM block reserved BEFORE the framebuffer
// corrupts the system PSRAM heap. multi_heap is Espressif's robust, internally
// locked allocator (the same one the PNG decode arena uses without trouble) —
// PSRAM placement (DMA-safe) AND a non-buggy allocator (map-safe).
//
// The block is reserved pre-framebuffer by crowpanel_lvgl_alloc_reserve()
// (called from display_init, next to the arena reserve). lv_mem_init(), invoked
// later by lv_init(), registers the multi_heap over that already-clean block.

#if defined(CROWPANEL_DHE04005D) && defined(ARCH_ESP32P4)

#include <stddef.h>
#include <stdint.h>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <multi_heap.h>

#include "lvgl.h"

// The unwrapped aligned alloc (bypasses PsramAllocGuard) so the fallback
// reservation draws from real PSRAM while the system heap is still pristine.
extern "C" void *__real_heap_caps_aligned_alloc(size_t alignment, size_t size, uint32_t caps);

// The PNG decode arena's multi_heap. In the normal path LVGL shares THIS heap
// (see PngDecodeArena.cpp) so LVGL structs and decoded image buffers live in one
// heap — a decoded buffer can never be freed into a different heap than it came
// from. Returns nullptr if the arena reservation failed, in which case we fall
// back to a private block below.
extern "C" void *crowpanel_shared_heap(void);

#ifndef LVGL_HEAP_KB
#define LVGL_HEAP_KB 6144 // fallback private heap size (only if arena missing)
#endif

namespace {
const char *TAG = "lvgl_alloc";
multi_heap_handle_t s_heap = nullptr;
void *s_block = nullptr; // only used by the private fallback path
size_t s_block_size = 0;
size_t s_max_used = 0;
bool s_shared = false; // true: s_heap is the arena's heap (do not free/re-register)
} // namespace

extern "C" void crowpanel_lvgl_alloc_reserve(void)
{
    // No-op in the shared-heap path: the arena reservation (done first in
    // display_init) provides the heap. Kept as a symbol for display_init and as
    // the private-fallback reservation if the arena is unavailable.
    if (s_block || crowpanel_shared_heap())
        return;
    const size_t bytes = (size_t)LVGL_HEAP_KB * 1024u;
    s_block = __real_heap_caps_aligned_alloc(32, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_block_size = s_block ? bytes : 0;
    ESP_LOGI(TAG, "fallback: reserved %u KB private PSRAM heap %s", (unsigned)(bytes / 1024),
             s_block ? "ok" : "FAILED");
}

extern "C" {

void lv_mem_init(void)
{
    if (s_heap)
        return;
    // Preferred: share the PNG arena's multi_heap so there is a single heap for
    // both LVGL and decoded image buffers (no cross-heap frees).
    s_heap = static_cast<multi_heap_handle_t>(crowpanel_shared_heap());
    if (s_heap) {
        s_shared = true;
        ESP_LOGI(TAG, "LVGL sharing PNG arena multi_heap (unified heap)");
        return;
    }
    // Fallback: private block (arena reservation must have failed).
    if (!s_block)
        crowpanel_lvgl_alloc_reserve();
    if (s_block)
        s_heap = multi_heap_register(s_block, s_block_size);
    if (!s_heap)
        ESP_LOGE(TAG, "no heap for LVGL (arena missing and private reserve failed)");
}

void lv_mem_deinit(void)
{
    // Drop the heap handle only. The shared arena heap is owned by
    // PngDecodeArena; the private fallback block stays reserved for re-init.
    s_heap = nullptr;
}

void *lv_malloc_core(size_t size)
{
    return s_heap ? multi_heap_malloc(s_heap, size) : nullptr;
}

void *lv_realloc_core(void *p, size_t new_size)
{
    return s_heap ? multi_heap_realloc(s_heap, p, new_size) : nullptr;
}

void lv_free_core(void *p)
{
    if (s_heap && p)
        multi_heap_free(s_heap, p);
}

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p)
{
    if (!mon_p)
        return;
    lv_memzero(mon_p, sizeof(lv_mem_monitor_t));
    if (!s_heap)
        return;
    multi_heap_info_t info;
    multi_heap_get_info(s_heap, &info);
    // In the shared-heap path s_block_size is 0; derive capacity from the heap.
    const size_t total = s_block_size ? s_block_size : (info.total_free_bytes + info.total_allocated_bytes);
    mon_p->total_size = total;
    mon_p->free_cnt = info.free_blocks;
    mon_p->free_size = info.total_free_bytes;
    mon_p->free_biggest_size = info.largest_free_block;
    mon_p->used_cnt = info.allocated_blocks;
    const size_t used = total - info.total_free_bytes;
    if (used > s_max_used)
        s_max_used = used;
    mon_p->max_used = s_max_used;
    mon_p->used_pct = total ? (uint8_t)((used * 100) / total) : 0;
    // Fragmentation: how much of free memory is NOT in the largest block.
    mon_p->frag_pct = info.total_free_bytes
                          ? (uint8_t)(100 - (info.largest_free_block * 100) / info.total_free_bytes)
                          : 0;
}

lv_result_t lv_mem_test_core(void)
{
    if (!s_heap)
        return LV_RESULT_INVALID;
    return multi_heap_check(s_heap, false) ? LV_RESULT_OK : LV_RESULT_INVALID;
}

} // extern "C"

#endif // CROWPANEL_DHE04005D && ARCH_ESP32P4
