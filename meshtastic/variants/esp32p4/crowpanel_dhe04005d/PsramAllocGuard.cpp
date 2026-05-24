#if defined(CROWPANEL_DHE04005D) && defined(ARCH_ESP32P4)

#include <stddef.h>
#include <stdint.h>
#include <esp_heap_caps.h>
#include <esp_log.h>

extern "C" {

void *__real_heap_caps_malloc(size_t size, uint32_t caps);
void *__real_heap_caps_calloc(size_t n, size_t size, uint32_t caps);
void *__real_heap_caps_realloc(void *ptr, size_t size, uint32_t caps);
void *__real_heap_caps_aligned_alloc(size_t alignment, size_t size, uint32_t caps);
void __real_free(void *ptr);

bool png_decode_arena_try_free_ptr(void *ptr);

static volatile bool s_armed = false;
static volatile uint32_t s_redirected = 0;

void psram_alloc_guard_arm()
{
    s_armed = true;
    ESP_LOGI("psram_guard", "armed: SPIRAM-only allocs from app-linked code "
                            "fall back to internal RAM (lwip TCP buffers "
                            "are NOT covered — see header)");
}

uint32_t psram_alloc_guard_redirect_count()
{
    return s_redirected;
}

static inline bool should_redirect(uint32_t caps)
{
    if (!s_armed) return false;
    if (!(caps & MALLOC_CAP_SPIRAM)) return false;
    if (caps & MALLOC_CAP_INTERNAL) return false;
    return true;
}

static inline uint32_t redirected_caps(uint32_t caps)
{
    // Preserve DMA requirement when requested, but force allocation to internal RAM
    // so we avoid touching corrupted SPIRAM free-lists post-RGB-FB init.
    uint32_t out = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    if (caps & MALLOC_CAP_DMA) {
        out |= MALLOC_CAP_DMA;
    }
    return out;
}

void *__wrap_heap_caps_malloc(size_t size, uint32_t caps)
{
    if (should_redirect(caps)) {
        s_redirected++;
        return __real_heap_caps_malloc(size, redirected_caps(caps));
    }
    return __real_heap_caps_malloc(size, caps);
}

void *__wrap_heap_caps_calloc(size_t n, size_t size, uint32_t caps)
{
    if (should_redirect(caps)) {
        s_redirected++;
        return __real_heap_caps_calloc(n, size, redirected_caps(caps));
    }
    return __real_heap_caps_calloc(n, size, caps);
}

void *__wrap_heap_caps_realloc(void *ptr, size_t size, uint32_t caps)
{
    if (should_redirect(caps)) {
        s_redirected++;
        return __real_heap_caps_realloc(ptr, size, redirected_caps(caps));
    }
    return __real_heap_caps_realloc(ptr, size, caps);
}

void *__wrap_heap_caps_aligned_alloc(size_t alignment, size_t size, uint32_t caps)
{
    if (should_redirect(caps)) {
        s_redirected++;
        return __real_heap_caps_aligned_alloc(alignment, size, redirected_caps(caps));
    }
    return __real_heap_caps_aligned_alloc(alignment, size, caps);
}

void __wrap_free(void *ptr)
{
    if (!ptr)
        return;
    if (png_decode_arena_try_free_ptr(ptr))
        return;
    __real_free(ptr);
}

}

#endif
