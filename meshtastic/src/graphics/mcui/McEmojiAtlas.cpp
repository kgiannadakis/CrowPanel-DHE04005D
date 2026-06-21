#if HAS_TFT && USE_MCUI

#include "McEmojiAtlas.h"

#include "board_config.h"

// NOTE: this runs from initVariant() (app_main, before setup()), where
// Meshtastic's LOG_*/RedirectablePrint is not yet initialized and crashes.
// Use IDF ESP_LOG* here, exactly like the rest of initVariant.
#include <driver/sdmmc_host.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>

static const char *EMOJI_TAG = "emoji";

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

namespace {

// --- Bundled test fallback (3 solid-color glyphs, planar RGB565A8) -----------
constexpr int TEST_GLYPH_PX = 20;
const uint32_t kTestCodepoints[] = {
    0x2764,  // heart
    0x1F44D, // thumbs up
    0x1F600, // grinning face
};
constexpr int kTestCount = sizeof(kTestCodepoints) / sizeof(kTestCodepoints[0]);

struct AtlasSlot {
    uint16_t glyph_px;
    uint32_t glyph_count;
    uint32_t *codepoints;       // sorted ascending
    uint8_t *pixels;            // glyph_count * glyph_px^2 * 3, planar [RGB565][A8] per glyph
    lv_image_dsc_t *descriptors; // glyph_count descriptors into pixels
    bool from_sd;
};

AtlasSlot s_slots[EMOJI_SIZE_COUNT] = {};
sdmmc_card_t *s_sd_card = nullptr;

void *psram_alloc(size_t bytes)
{
    void *p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p)
        p = heap_caps_malloc(bytes, MALLOC_CAP_DEFAULT);
    return p;
}

// De-interleave one glyph from [lo,hi,a] per pixel to planar [RGB565 plane][A8 plane],
// in place. glyph_px <= 32 (atlas tiers are 20 and 32).
void repack_glyph_planar(uint8_t *g, uint16_t glyph_px)
{
    const size_t n = (size_t)glyph_px * glyph_px;
    uint8_t temp[32 * 32 * 3];
    if (n * 3 > sizeof(temp))
        return;
    memcpy(temp, g, n * 3);
    uint8_t *rgb = g;        // 2*n bytes
    uint8_t *alpha = g + n * 2; // n bytes
    for (size_t i = 0; i < n; ++i) {
        rgb[i * 2 + 0] = temp[i * 3 + 0];
        rgb[i * 2 + 1] = temp[i * 3 + 1];
        alpha[i] = temp[i * 3 + 2];
    }
}

bool build_descriptors(AtlasSlot &slot)
{
    const size_t glyph_bytes = (size_t)slot.glyph_px * slot.glyph_px * 3;
    slot.descriptors = (lv_image_dsc_t *)psram_alloc(sizeof(lv_image_dsc_t) * slot.glyph_count);
    if (!slot.descriptors) {
        ESP_LOGW(EMOJI_TAG, "descriptor pool alloc failed");
        return false;
    }
    memset(slot.descriptors, 0, sizeof(lv_image_dsc_t) * slot.glyph_count);

    for (uint32_t i = 0; i < slot.glyph_count; ++i) {
        lv_image_dsc_t *d = &slot.descriptors[i];
        d->header.magic = LV_IMAGE_HEADER_MAGIC;
        d->header.cf = LV_COLOR_FORMAT_RGB565A8;
        d->header.flags = 0;
        d->header.w = slot.glyph_px;
        d->header.h = slot.glyph_px;
        d->header.stride = (uint16_t)(slot.glyph_px * 2); // RGB565 plane row stride
        d->data_size = (uint32_t)glyph_bytes;
        d->data = slot.pixels + (size_t)i * glyph_bytes;
    }
    return true;
}

void fill_test_glyph_planar(uint8_t *dst, int px, uint8_t r, uint8_t g, uint8_t b)
{
    const uint16_t c = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    const size_t n = (size_t)px * px;
    for (size_t i = 0; i < n; ++i) {
        dst[i * 2 + 0] = (uint8_t)(c & 0xFF);
        dst[i * 2 + 1] = (uint8_t)(c >> 8);
    }
    memset(dst + n * 2, 0xFF, n); // full alpha
}

bool load_bundled_test(AtlasSlot &slot)
{
    const size_t glyph_bytes = (size_t)TEST_GLYPH_PX * TEST_GLYPH_PX * 3;
    slot.glyph_px = TEST_GLYPH_PX;
    slot.glyph_count = kTestCount;
    slot.from_sd = false;

    slot.codepoints = (uint32_t *)psram_alloc(sizeof(uint32_t) * kTestCount);
    if (!slot.codepoints)
        return false;
    memcpy(slot.codepoints, kTestCodepoints, sizeof(uint32_t) * kTestCount);

    slot.pixels = (uint8_t *)psram_alloc(glyph_bytes * kTestCount);
    if (!slot.pixels) {
        heap_caps_free(slot.codepoints);
        slot.codepoints = nullptr;
        return false;
    }
    fill_test_glyph_planar(slot.pixels + 0 * glyph_bytes, TEST_GLYPH_PX, 0xD0, 0x20, 0x20); // heart
    fill_test_glyph_planar(slot.pixels + 1 * glyph_bytes, TEST_GLYPH_PX, 0x20, 0x60, 0xD0); // thumbs up
    fill_test_glyph_planar(slot.pixels + 2 * glyph_bytes, TEST_GLYPH_PX, 0xE0, 0xC0, 0x20); // grin

    return build_descriptors(slot);
}

struct AtlasFileHeader {
    char magic[4];
    uint16_t version;
    uint16_t glyph_px;
    uint32_t glyph_count;
    uint32_t reserved;
} __attribute__((packed));

bool load_sd_atlas(AtlasSlot &slot, const char *path, uint16_t expected_px)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;

    AtlasFileHeader hdr;
    if (fread(&hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        fclose(f);
        return false;
    }
    if (memcmp(hdr.magic, "EMA1", 4) != 0 || hdr.version != 1 || hdr.glyph_px != expected_px ||
        hdr.glyph_count == 0 || hdr.glyph_count > 4096) {
        ESP_LOGW(EMOJI_TAG, "SD atlas %s header invalid", path);
        fclose(f);
        return false;
    }

    const size_t cp_bytes = sizeof(uint32_t) * hdr.glyph_count;
    const size_t glyph_bytes = (size_t)hdr.glyph_px * hdr.glyph_px * 3;
    const size_t pixel_bytes = (size_t)hdr.glyph_count * glyph_bytes;

    uint32_t *cps = (uint32_t *)psram_alloc(cp_bytes);
    uint8_t *px = (uint8_t *)psram_alloc(pixel_bytes);
    if (!cps || !px) {
        if (cps)
            heap_caps_free(cps);
        if (px)
            heap_caps_free(px);
        ESP_LOGW(EMOJI_TAG, "SD atlas alloc failed (%u bytes)", (unsigned)pixel_bytes);
        fclose(f);
        return false;
    }

    if (fread(cps, 1, cp_bytes, f) != cp_bytes || fread(px, 1, pixel_bytes, f) != pixel_bytes) {
        heap_caps_free(cps);
        heap_caps_free(px);
        ESP_LOGW(EMOJI_TAG, "SD atlas %s short read", path);
        fclose(f);
        return false;
    }
    fclose(f);

    // Convert each glyph from interleaved (v8 layout on disk) to planar RGB565A8.
    for (uint32_t i = 0; i < hdr.glyph_count; ++i)
        repack_glyph_planar(px + (size_t)i * glyph_bytes, hdr.glyph_px);

    // Replace the bundled fallback allocations with the SD atlas.
    if (slot.codepoints)
        heap_caps_free(slot.codepoints);
    if (slot.pixels)
        heap_caps_free(slot.pixels);
    if (slot.descriptors)
        heap_caps_free(slot.descriptors);

    slot.glyph_px = hdr.glyph_px;
    slot.glyph_count = hdr.glyph_count;
    slot.codepoints = cps;
    slot.pixels = px;
    slot.descriptors = nullptr;
    slot.from_sd = true;

    if (!build_descriptors(slot))
        return false;

    ESP_LOGI(EMOJI_TAG, "SD atlas loaded %s - %u glyphs, %ux%u", path, (unsigned)hdr.glyph_count,
             (unsigned)hdr.glyph_px, (unsigned)hdr.glyph_px);
    return true;
}

// Mount the SD card at /sdcard (1-bit SDMMC). Returns true if mounted.
// On CrowPanel P4 this is intentionally done before ESP-Hosted WiFi starts and
// left mounted, so maps never reconfigure SDMMC while the hosted SDIO link is
// already running.
bool emoji_sd_mount(void)
{
    struct stat st = {};
    if (stat("/sdcard", &st) == 0 && (st.st_mode & S_IFDIR))
        return true; // already mounted

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {};
    mount_cfg.format_if_mount_failed = false;
    mount_cfg.max_files = 8;
    mount_cfg.allocation_unit_size = 16 * 1024;

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = 10000;

    sdmmc_slot_config_t slot_cfg = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_cfg.clk = (gpio_num_t)SD_GPIO_MMC_CLK;
    slot_cfg.cmd = (gpio_num_t)SD_GPIO_MMC_CMD;
    slot_cfg.d0 = (gpio_num_t)SD_GPIO_MMC_D0;
    slot_cfg.width = 1;
    slot_cfg.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_err_t err = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot_cfg, &mount_cfg, &s_sd_card);
    if (err != ESP_OK) {
        ESP_LOGI(EMOJI_TAG, "SD not mounted (%s) - using bundled glyphs only", esp_err_to_name(err));
        s_sd_card = nullptr;
        return false;
    }
    return true;
}

} // namespace

bool emoji_atlas_init(void)
{
    if (!load_bundled_test(s_slots[EMOJI_SIZE_SMALL]) || !load_bundled_test(s_slots[EMOJI_SIZE_BIG])) {
        ESP_LOGW(EMOJI_TAG, "bundled test atlas alloc failed");
        return false;
    }

    // Mount the SD card before ESP-Hosted WiFi starts and keep it mounted.
    // Maps reuse the same mount; mounting SD later while WiFi is live can
    // disturb ESP-Hosted SDIO on this board.
    if (emoji_sd_mount()) {
        load_sd_atlas(s_slots[EMOJI_SIZE_SMALL], "/sdcard/emoji/emoji_atlas_20.bin", 20);
        load_sd_atlas(s_slots[EMOJI_SIZE_BIG], "/sdcard/emoji/emoji_atlas_32.bin", 32);
    }
    return true;
}

const lv_image_dsc_t *emoji_atlas_lookup(uint32_t codepoint, emoji_size_t size)
{
    if ((int)size < 0 || (int)size >= EMOJI_SIZE_COUNT)
        return nullptr;
    const AtlasSlot &slot = s_slots[size];
    if (!slot.codepoints || slot.glyph_count == 0 || !slot.descriptors)
        return nullptr;

    int lo = 0, hi = (int)slot.glyph_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        uint32_t mcp = slot.codepoints[mid];
        if (mcp == codepoint)
            return &slot.descriptors[mid];
        if (mcp < codepoint)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return nullptr;
}

bool emoji_atlas_has(uint32_t codepoint)
{
    return emoji_atlas_lookup(codepoint, EMOJI_SIZE_SMALL) != nullptr ||
           emoji_atlas_lookup(codepoint, EMOJI_SIZE_BIG) != nullptr;
}

uint16_t emoji_atlas_glyph_px(emoji_size_t size)
{
    if ((int)size < 0 || (int)size >= EMOJI_SIZE_COUNT)
        return 0;
    return s_slots[size].glyph_px;
}

#endif
