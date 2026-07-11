#if defined(CROWPANEL_DHE04005D) && defined(ARCH_ESP32P4)

// ---------------------------------------------------------------------------
// PNG decode arena (CrowPanel DHE04005D / ESP32-P4)
// ---------------------------------------------------------------------------
//
// Problem this solves:
//   * A 256x256 map tile decodes to a 256 KB ARGB8888 buffer, and lodepng needs
//     a few hundred KB of scratch on top of that. At runtime, neither heap can
//     supply it on this board:
//       - ordinary malloc is pinned to internal RAM (main.cpp calls
//         heap_caps_malloc_extmem_enable(SIZE_MAX)) which is far too small;
//       - the *system* PSRAM heap's TLSF free-list is corrupted after the RGB
//         framebuffer init, so allocating from it asserts in block_locate_free.
//
// Approach (chosen "boot-time PSRAM arena"):
//   1. Reserve one large PSRAM block *before* esp_lcd_panel_init() runs, while
//      the system PSRAM heap is still clean (png_decode_arena_reserve()).
//   2. Put a private multi_heap over that block. All later decode allocations
//      are served from this private heap, whose metadata lives inside the
//      pre-reserved block, so they never touch the corrupted system TLSF.
//   3. Route BOTH decode allocation paths into the arena:
//        - lodepng scratch  -> lodepng_malloc/realloc/free (this file; enabled
//          by -DLODEPNG_NO_COMPILE_ALLOCATORS so LVGL's lodepng.c calls ours);
//        - the decoded ARGB8888 output buffer -> LVGL's image-cache draw-buf
//          handlers (png_decode_arena_install_handlers(), after lv_init()).
//
// Lifetime / pairing: the output buffer is alloc'd and freed through the same
// (arena) draw-buf handlers; lodepng scratch is alloc'd and freed through the
// same (arena) lodepng_* functions. The small lv_draw_buf_t header struct is
// left on internal lv_malloc/lv_free (tiny). No buffer crosses allocators.
//
// Threading: all PNG decoding happens inside the single LVGL task (under the
// display's LVGL mutex), so arena access is serialized; multi_heap needs no
// extra lock here.
//
// Graceful failure: if the arena can't be reserved, the allocators return NULL
// and decoding fails cleanly (tile simply not drawn) instead of crashing.

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <multi_heap.h>
#include <stddef.h>
#include <stdint.h>

#include <lvgl.h>
#include <draw/lv_draw_buf_private.h>

static const char *TAG = "png_arena";

// 12 MB: holds the image cache (LV_CACHE_DEF_SIZE = 6 MB) plus a tile's decode
// in flight (256 KB output + a few hundred KB lodepng scratch) with generous
// fragmentation headroom. Kept well under PSRAM's 32 MB so the single RGB
// framebuffer (~768 KB), LVGL partial draw buffers, and ESP-Hosted WiFi/MQTT
// have ample room (~20 MB left). Raise if larger viewports need more cache.
// Sized to hold BOTH the decoded-tile image cache AND all of LVGL's general
// allocations (widget tree, draw tasks, decoder instances). Since 2.7.26 the
// LVGL allocator is routed to THIS same multi_heap (see crowpanel_lvgl_alloc:
// crowpanel_shared_heap()) so that one robust, integrity-checked PSRAM heap
// backs everything — a decoded buffer can never be freed into a different heap
// than it was allocated from (the cross-heap free that corrupted the split
// two-heap layout). Budget: ~9.5 MB image cache + ~0.5 MB UI + decode scratch.
#ifndef PNG_ARENA_SIZE
#define PNG_ARENA_SIZE (16u * 1024u * 1024u)
#endif

// Unwrapped original, provided by the linker via -Wl,--wrap=heap_caps_malloc
// (see PsramAllocGuard.cpp). Using __real_* bypasses the guard so this one
// reservation reaches real PSRAM. It is called before the framebuffer init, so
// the system PSRAM heap is still intact at that moment.
extern "C" void *__real_heap_caps_malloc(size_t size, uint32_t caps);
extern "C" void *__real_heap_caps_aligned_alloc(size_t alignment, size_t size, uint32_t caps);

static multi_heap_handle_t s_arena = nullptr;
static uint8_t * s_region_base = nullptr;
static size_t s_region_size = 0;

// --- decoded-output buffer handlers (installed into image_cache handlers) ----

static void *arena_buf_malloc(size_t size_bytes, lv_color_format_t cf)
{
    (void)cf;
    // Mirror LVGL's default buf_malloc: over-allocate so the pointer can be
    // aligned to LV_DRAW_BUF_ALIGN afterwards.
    size_bytes += LV_DRAW_BUF_ALIGN - 1;
    if (s_arena)
        return multi_heap_malloc(s_arena, size_bytes);
    return nullptr;
}

static void arena_buf_free(void *buf)
{
    if (buf && s_arena)
        multi_heap_free(s_arena, buf);
}

extern "C" {

// Reserve the PSRAM arena. MUST be called while the system PSRAM heap is still
// clean, i.e. BEFORE esp_lcd_panel_init().
void png_decode_arena_reserve(void)
{
    if (s_arena)
        return;
    void *region = __real_heap_caps_malloc(PNG_ARENA_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!region) {
        ESP_LOGE(TAG, "reserve failed: no %u KB PSRAM block", (unsigned)(PNG_ARENA_SIZE / 1024));
        return;
    }
    s_arena = multi_heap_register(region, PNG_ARENA_SIZE);
    if (!s_arena) {
        ESP_LOGE(TAG, "multi_heap_register failed");
        return;
    }
    s_region_base = static_cast<uint8_t *>(region);
    s_region_size = PNG_ARENA_SIZE;
    ESP_LOGI(TAG, "reserved %u KB PSRAM @ %p (free=%u)", (unsigned)(PNG_ARENA_SIZE / 1024), region,
             (unsigned)multi_heap_free_size(s_arena));
}

// Expose the arena's multi_heap so the LVGL custom allocator can share it.
// Returns nullptr until png_decode_arena_reserve() has run. Sharing one heap
// for LVGL structs AND decoded image buffers makes a cross-heap free (which
// corrupts a heap and was the root cause of the map-screen freeze/crash)
// structurally impossible.
void *crowpanel_shared_heap(void)
{
    return static_cast<void *>(s_arena);
}

// Install the arena allocator into LVGL's image-cache draw-buf handlers so the
// decoded ARGB8888 output buffer comes from the arena. MUST be called AFTER
// lv_init() (which initializes the default handlers).
void png_decode_arena_install_handlers(void)
{
    if (!s_arena) {
        ESP_LOGW(TAG, "no arena; image decode left on default (internal) heap");
        return;
    }
    lv_draw_buf_handlers_t *h = lv_draw_buf_get_image_handlers();
    if (!h)
        return;
    h->buf_malloc_cb = arena_buf_malloc;
    h->buf_free_cb = arena_buf_free;
    ESP_LOGI(TAG, "image decode draw-buf handlers routed to PSRAM arena");
}

bool png_decode_arena_owns_ptr(const void *ptr)
{
    if (!ptr || !s_region_base || s_region_size == 0)
        return false;
    const auto p = static_cast<const uint8_t *>(ptr);
    return (p >= s_region_base) && (p < (s_region_base + s_region_size));
}

bool png_decode_arena_try_free_ptr(void *ptr)
{
    if (!ptr || !s_arena)
        return false;
    if (!png_decode_arena_owns_ptr(ptr))
        return false;
    multi_heap_free(s_arena, ptr);
    return true;
}

// General-purpose allocation from the arena for large, long-lived UI assets
// (emoji atlas glyphs etc.). Once the PSRAM guard is armed, plain SPIRAM
// allocations get redirected to INTERNAL RAM — a ~180 KB asset would consume
// the entire DMA/internal pool (starving ESP-Hosted until its SDIO driver
// asserts). Same serialization assumption as the decode handlers: callers run
// on the UI task. Returns NULL if the arena is unavailable or full.
void *png_decode_arena_malloc(size_t size)
{
    if (!s_arena)
        return nullptr;
    return multi_heap_malloc(s_arena, size);
}

// --- lodepng scratch allocators (paired with -DLODEPNG_NO_COMPILE_ALLOCATORS) -

void *lodepng_malloc(size_t size)
{
    if (s_arena)
        return multi_heap_malloc(s_arena, size);
    return nullptr;
}

void *lodepng_realloc(void *ptr, size_t new_size)
{
    if (s_arena)
        return multi_heap_realloc(s_arena, ptr, new_size);
    return nullptr;
}

void lodepng_free(void *ptr)
{
    if (ptr && s_arena)
        multi_heap_free(s_arena, ptr);
}

} // extern "C"

#endif // CROWPANEL_DHE04005D && ARCH_ESP32P4
