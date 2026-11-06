#include <stdint.h>
#include <stddef.h>
#define ESP_OK 0
#define ESP_IMAGE_HEADER_MAGIC 0xE9
#define ESP_IMAGE_MAX_SEGMENTS 16
#define ESP_IMAGE_VERIFY_SILENT 1
#define ESP_PARTITION_TYPE_APP 0
#define ESP_PARTITION_SUBTYPE_APP_FACTORY 0
#define ESP_PARTITION_TYPE_DATA 1
#define ESP_PARTITION_SUBTYPE_DATA_OTA 0
struct esp_partition_t { uint32_t address, size; };
struct esp_partition_pos_t { uint32_t address, size; };
struct esp_image_header_t { uint8_t magic, segment_count, rest[22]; };
struct esp_image_segment_header_t { uint32_t load_addr, data_len; };
struct esp_image_metadata_t { uint32_t unused; };
static uint8_t flash[128];
static esp_partition_t factory = {0x10000, sizeof(flash)}, ota = {0xe000, 8192};
static bool present, read_error;
static int verify_result, verified, erased, reads;

extern "C" void* memcpy(void* dst, const void* src, size_t n) {
    auto* d = static_cast<uint8_t*>(dst);
    const auto* s = static_cast<const uint8_t*>(src);
    for (size_t i = 0; i < n; ++i) d[i] = s[i];
    return dst;
}
static const esp_partition_t* esp_partition_find_first(int type, int, const char*) {
    return type == ESP_PARTITION_TYPE_APP ? (present ? &factory : nullptr) : &ota;
}
static int esp_partition_read(const esp_partition_t* part, size_t offset, void* data, size_t n) {
    ++reads;
    if (read_error || part != &factory || offset > part->size || n > part->size - offset) return -1;
    memcpy(data, flash + offset, n);
    return ESP_OK;
}
static int esp_image_verify(int, const esp_partition_pos_t* pos, esp_image_metadata_t*) {
    ++verified;
    return pos->address == factory.address && pos->size == factory.size ? verify_result : -1;
}
static int esp_partition_erase_range(const esp_partition_t* part, size_t offset, size_t len) {
    if (part != &ota || offset != 0 || len != ota.size) return -1;
    ++erased;
    return ESP_OK;
}
// INSERT_PRODUCTION_METHODS

static void reset() {
    for (auto& b : flash) b = 0;
    present = true; read_error = false;
    verify_result = ESP_OK; verified = erased = reads = 0;
    factory.size = sizeof(flash);
    auto* header = reinterpret_cast<esp_image_header_t*>(flash);
    header->magic = ESP_IMAGE_HEADER_MAGIC; header->segment_count = 1;
    auto* segment = reinterpret_cast<esp_image_segment_header_t*>(flash + sizeof(*header));
    segment->load_addr = 0x4ff00000; segment->data_len = 16;
}

extern "C" uint32_t run_tests() {
    reset(); initVariant(); if (verified != 1 || erased != 1) return 1;
    reset(); present = false; initVariant(); if (reads || verified || erased) return 2;
    reset(); read_error = true; initVariant(); if (verified || erased) return 3;
    reset(); flash[0] = 0xff; initVariant(); if (verified || erased) return 4;
    reset(); flash[1] = 0; initVariant(); if (verified || erased) return 5;
    reset(); flash[1] = 17; initVariant(); if (verified || erased) return 6;
    reset(); factory.size = 23; initVariant(); if (reads || verified || erased) return 7;
    reset(); factory.size = 30; initVariant(); if (reads != 1 || verified || erased) return 8;
    reset();
    auto* segment = reinterpret_cast<esp_image_segment_header_t*>(flash + sizeof(esp_image_header_t));
    segment->data_len = 0x45474150; initVariant(); if (reads != 2 || verified || erased) return 9;
    reset(); segment->data_len = 100; initVariant(); if (verified || erased) return 10;
    reset(); segment->data_len = 17; initVariant(); if (verified || erased) return 11;
    reset(); flash[1] = 2; segment->data_len = 96; initVariant(); if (reads != 2 || verified || erased) return 12;
    reset(); verify_result = -1; initVariant(); if (verified != 1 || erased) return 13;
    return 0;
}
