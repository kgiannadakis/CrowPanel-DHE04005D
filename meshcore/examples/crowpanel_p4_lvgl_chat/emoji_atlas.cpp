// ============================================================
// emoji_atlas.cpp
// ------------------------------------------------------------
// See emoji_atlas.h for the on-disk format spec. Summary:
//   header + uint32 codepoints[] + RGB565A8 pixel data[]
//
// At boot we allocate, once per atlas size tier:
//   * uint32 codepoints[N]  (~1.8 KB for 450 entries)
//   * uint8  pixels[N * glyph_px * glyph_px * 3]  (720 KB at 20px,
//                                                  1.8 MB at 32px)
//   * lv_img_dsc_t dsc[N]   (~24 bytes × N — descriptor pool)
// All in PSRAM. Each lv_img_dsc_t's data pointer is set to the
// matching slice of the pixel array.
//
// Lookup is O(log N) via binary search on the sorted codepoints[].
// ============================================================

#include "emoji_atlas.h"
#include "sd_storage.h"
#include "utils.h"     // serialmon_append

#include <esp_heap_caps.h>
#include <string.h>
#include <stdio.h>

// --- Bundled test fallback ----------------------------------------
// A three-glyph atlas baked into flash so the rendering pipeline is
// verifiable before the user has run build_emoji_atlas.py. Each
// glyph is a 20×20 solid-color block in RGB565A8 format (2 bytes
// RGB565 + 1 byte alpha per pixel = 3 bytes × 400 = 1200 bytes).

#define TEST_GLYPH_PX 20
#define TEST_GLYPH_BYTES (TEST_GLYPH_PX * TEST_GLYPH_PX * 3)

static const uint32_t kTestCodepoints[] = {
    0x2764,   // ❤  red heart
    0x1F44D,  // 👍  thumbs up
    0x1F600,  // 😀  grinning face
};
static const int kTestCount = sizeof(kTestCodepoints) / sizeof(kTestCodepoints[0]);

// RGB565 encoding of primary colors.
static inline void rgb_to_565(uint8_t r, uint8_t g, uint8_t b, uint8_t* lo, uint8_t* hi) {
    uint16_t c = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    *lo = (uint8_t)(c & 0xFF);
    *hi = (uint8_t)(c >> 8);
}

// Fill a 20×20 RGB565A8 buffer with a single color + full alpha.
static void test_fill_glyph(uint8_t* dst, uint8_t r, uint8_t g, uint8_t b) {
    uint8_t lo, hi;
    rgb_to_565(r, g, b, &lo, &hi);
    for (int i = 0; i < TEST_GLYPH_PX * TEST_GLYPH_PX; ++i) {
        dst[i * 3 + 0] = lo;
        dst[i * 3 + 1] = hi;
        dst[i * 3 + 2] = 0xFF;
    }
}

// --- Loaded atlas state (one per size tier) -----------------------

struct AtlasSlot {
    uint16_t       glyph_px;      // 20 or 32
    uint32_t       glyph_count;   // number of entries
    uint32_t*      codepoints;    // sorted ascending
    uint8_t*       pixels;        // glyph_count * glyph_px^2 * 3 bytes
    lv_img_dsc_t*  descriptors;   // glyph_count descriptors into pixels
    bool           from_sd;       // true if loaded from SD, false if bundled test
};

static AtlasSlot s_slots[EMOJI_SIZE_COUNT] = {};

// --- PSRAM allocation helper --------------------------------------

static void* psram_alloc(size_t bytes) {
    void* p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_malloc(bytes, MALLOC_CAP_DEFAULT);  // fallback
    return p;
}

// --- Descriptor initialisation ------------------------------------
// Build one lv_img_dsc_t per glyph in the slot, pointing into the
// slot's pixel buffer. Done after codepoints + pixels are loaded.

static bool build_descriptors(AtlasSlot& slot) {
    const size_t glyph_bytes = (size_t)slot.glyph_px * slot.glyph_px * 3;
    slot.descriptors = (lv_img_dsc_t*)psram_alloc(
        sizeof(lv_img_dsc_t) * slot.glyph_count);
    if (!slot.descriptors) {
        serialmon_append("emoji: descriptor pool alloc failed");
        return false;
    }
    memset(slot.descriptors, 0, sizeof(lv_img_dsc_t) * slot.glyph_count);

    for (uint32_t i = 0; i < slot.glyph_count; ++i) {
        lv_img_dsc_t* d = &slot.descriptors[i];
        d->header.always_zero = 0;
        d->header.w  = slot.glyph_px;
        d->header.h  = slot.glyph_px;
        d->header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
        d->data_size = glyph_bytes;
        d->data      = slot.pixels + i * glyph_bytes;
    }
    return true;
}

// --- Bundled test atlas loader ------------------------------------
// Populates a slot with the 3-glyph solid-color test atlas. Only
// the SMALL tier gets these; BIG tier is left empty (keyboard
// emojis will just not render in color on a card-less first boot).

static bool load_bundled_test(AtlasSlot& slot) {
    slot.glyph_px    = TEST_GLYPH_PX;
    slot.glyph_count = kTestCount;
    slot.from_sd     = false;

    slot.codepoints = (uint32_t*)psram_alloc(sizeof(uint32_t) * kTestCount);
    if (!slot.codepoints) return false;
    memcpy(slot.codepoints, kTestCodepoints, sizeof(uint32_t) * kTestCount);

    slot.pixels = (uint8_t*)psram_alloc(TEST_GLYPH_BYTES * kTestCount);
    if (!slot.pixels) { heap_caps_free(slot.codepoints); slot.codepoints = nullptr; return false; }

    // Index order matches kTestCodepoints order above.
    test_fill_glyph(slot.pixels + 0 * TEST_GLYPH_BYTES, 0xD0, 0x20, 0x20);  // red ❤
    test_fill_glyph(slot.pixels + 1 * TEST_GLYPH_BYTES, 0x20, 0x60, 0xD0);  // blue 👍
    test_fill_glyph(slot.pixels + 2 * TEST_GLYPH_BYTES, 0xE0, 0xC0, 0x20);  // yellow 😀

    if (!build_descriptors(slot)) return false;

    serialmon_append("emoji: test atlas loaded (3 glyphs, 20x20)");
    return true;
}

// --- SD atlas loader ----------------------------------------------

struct AtlasFileHeader {
    char     magic[4];
    uint16_t version;
    uint16_t glyph_px;
    uint32_t glyph_count;
    uint32_t reserved;
} __attribute__((packed));

static bool load_sd_atlas(AtlasSlot& slot, const char* path, uint16_t expected_px) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;

    AtlasFileHeader hdr;
    if (fread(&hdr, 1, sizeof(hdr), f) != sizeof(hdr)) { fclose(f); return false; }

    if (memcmp(hdr.magic, "EMA1", 4) != 0) {
        serialmon_append("emoji: SD atlas bad magic");
        fclose(f);
        return false;
    }
    if (hdr.version != 1) {
        serialmon_append("emoji: SD atlas version mismatch");
        fclose(f);
        return false;
    }
    if (hdr.glyph_px != expected_px) {
        char buf[96];
        snprintf(buf, sizeof(buf), "emoji: SD atlas %s has glyph_px=%u, expected %u",
                 path, (unsigned)hdr.glyph_px, (unsigned)expected_px);
        serialmon_append(buf);
        fclose(f);
        return false;
    }
    if (hdr.glyph_count == 0 || hdr.glyph_count > 4096) {
        serialmon_append("emoji: SD atlas glyph_count out of range");
        fclose(f);
        return false;
    }

    const size_t cp_bytes    = sizeof(uint32_t) * hdr.glyph_count;
    const size_t pixel_bytes = (size_t)hdr.glyph_count * hdr.glyph_px * hdr.glyph_px * 3;

    uint32_t* cps = (uint32_t*)psram_alloc(cp_bytes);
    uint8_t*  px  = (uint8_t*) psram_alloc(pixel_bytes);
    if (!cps || !px) {
        if (cps) heap_caps_free(cps);
        if (px)  heap_caps_free(px);
        serialmon_append("emoji: SD atlas alloc failed");
        fclose(f);
        return false;
    }

    if (fread(cps, 1, cp_bytes, f) != cp_bytes
        || fread(px, 1, pixel_bytes, f) != pixel_bytes) {
        heap_caps_free(cps);
        heap_caps_free(px);
        serialmon_append("emoji: SD atlas short read");
        fclose(f);
        return false;
    }
    fclose(f);

    // Any previous (bundled) allocations are freed so we own a clean slot.
    if (slot.codepoints)  { heap_caps_free(slot.codepoints);  slot.codepoints  = nullptr; }
    if (slot.pixels)      { heap_caps_free(slot.pixels);      slot.pixels      = nullptr; }
    if (slot.descriptors) { heap_caps_free(slot.descriptors); slot.descriptors = nullptr; }

    slot.glyph_px    = hdr.glyph_px;
    slot.glyph_count = hdr.glyph_count;
    slot.codepoints  = cps;
    slot.pixels      = px;
    slot.from_sd     = true;

    if (!build_descriptors(slot)) return false;

    char msg[96];
    snprintf(msg, sizeof(msg), "emoji: SD atlas loaded %s - %u glyphs, %ux%u",
             path, (unsigned)hdr.glyph_count, (unsigned)hdr.glyph_px, (unsigned)hdr.glyph_px);
    serialmon_append(msg);
    return true;
}

// --- Public API ---------------------------------------------------

bool emoji_atlas_init() {
    // Populate BOTH tiers with the bundled test atlas. Chat body is
    // 16pt which routes to the BIG tier; if only SMALL were loaded,
    // the test emojis would miss during first-boot validation.
    // load_sd_atlas() below frees these allocations and replaces
    // them if/when the real SD atlas is available.
    load_bundled_test(s_slots[EMOJI_SIZE_SMALL]);
    load_bundled_test(s_slots[EMOJI_SIZE_BIG]);

    if (sd_is_mounted()) {
        // Try to replace the test fallback with the real atlas.
        load_sd_atlas(s_slots[EMOJI_SIZE_SMALL],
                      "/sdcard/emoji/emoji_atlas_20.bin", 20);
        load_sd_atlas(s_slots[EMOJI_SIZE_BIG],
                      "/sdcard/emoji/emoji_atlas_32.bin", 32);
    } else {
        serialmon_append("emoji: no SD, using bundled test atlas only");
    }

    // Bring-up sanity: verify lookup finds each hardcoded test
    // glyph in each tier, and log dimensions + pixel pointer for
    // one sample. If any of these fail, the problem is on our side
    // (bad format / bad pointer) and there's no point debugging
    // LVGL's rendering pipeline.
    for (int tier = 0; tier < EMOJI_SIZE_COUNT; ++tier) {
        const char* tname = (tier == EMOJI_SIZE_SMALL) ? "small" : "big";
        for (int i = 0; i < kTestCount; ++i) {
            uint32_t cp = kTestCodepoints[i];
            const lv_img_dsc_t* d = emoji_atlas_lookup(cp, (emoji_size_t)tier);
            char buf[96];
            if (!d) {
                snprintf(buf, sizeof(buf),
                         "emoji sanity %s: U+%05X MISS",
                         tname, (unsigned)cp);
            } else if (i == 0) {
                // Only log details for the first glyph of each tier
                // to keep the log readable.
                snprintf(buf, sizeof(buf),
                         "emoji sanity %s: U+%05X wxh=%dx%d cf=%u data=%p sz=%u",
                         tname, (unsigned)cp,
                         (int)d->header.w, (int)d->header.h,
                         (unsigned)d->header.cf, d->data, (unsigned)d->data_size);
            } else {
                snprintf(buf, sizeof(buf), "emoji sanity %s: U+%05X HIT",
                         tname, (unsigned)cp);
            }
            serialmon_append(buf);
        }
    }
    return true;
}

bool emoji_atlas_has_sd(emoji_size_t size) {
    if ((int)size < 0 || (int)size >= EMOJI_SIZE_COUNT) return false;
    return s_slots[size].from_sd;
}

uint16_t emoji_atlas_glyph_px(emoji_size_t size) {
    if ((int)size < 0 || (int)size >= EMOJI_SIZE_COUNT) return 0;
    return s_slots[size].glyph_px;
}

bool emoji_atlas_has(uint32_t codepoint) {
    // "Has" here means present in EITHER tier — the sanitiser only
    // needs to know whether the codepoint will render at all, not
    // which tier ends up serving it.
    return emoji_atlas_lookup(codepoint, EMOJI_SIZE_SMALL) != nullptr
        || emoji_atlas_lookup(codepoint, EMOJI_SIZE_BIG)   != nullptr;
}

// Binary search over sorted codepoints[]. The bundled test atlas
// already satisfies this ordering; build_emoji_atlas.py emits
// entries sorted, so SD atlases do too.
const lv_img_dsc_t* emoji_atlas_lookup(uint32_t codepoint, emoji_size_t size) {
    if ((int)size < 0 || (int)size >= EMOJI_SIZE_COUNT) return nullptr;
    const AtlasSlot& slot = s_slots[size];
    if (!slot.codepoints || slot.glyph_count == 0) return nullptr;

    int lo = 0;
    int hi = (int)slot.glyph_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        uint32_t mcp = slot.codepoints[mid];
        if (mcp == codepoint) return &slot.descriptors[mid];
        if (mcp < codepoint) lo = mid + 1;
        else                 hi = mid - 1;
    }
    return nullptr;
}
