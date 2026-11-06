// =============================================================================
// crowpanel_dualboot.cpp
//
// Return to the factory selector on the next reset only if its complete image
// validates. A stale/oversize factory image must not strand a working OTA app.
//
// NOTE: this file used to also pre-mount SPIFFS on the "mcdata" partition,
// which was correct for the V11 build. The P4 chat build uses LittleFS on
// that same partition (see main.cpp's setup()). Mounting both formats on
// the same partition causes them to reformat each other on every boot,
// wiping channels/contacts/identity. Keep this file FS-agnostic.
// =============================================================================

#include <Arduino.h>
#include <esp_partition.h>
#include <esp_image_format.h>

static bool factory_image_fits(const esp_partition_t* factory) {
    if (!factory || factory->size < sizeof(esp_image_header_t)) return false;
    esp_image_header_t header = {};
    if (esp_partition_read(factory, 0, &header, sizeof(header)) != ESP_OK ||
        header.magic != ESP_IMAGE_HEADER_MAGIC || header.segment_count == 0 ||
        header.segment_count > ESP_IMAGE_MAX_SEGMENTS) return false;
    size_t offset = sizeof(header);
    for (uint8_t i = 0; i < header.segment_count; ++i) {
        if (factory->size - offset < sizeof(esp_image_segment_header_t)) return false;
        esp_image_segment_header_t segment = {};
        if (esp_partition_read(factory, offset, &segment, sizeof(segment)) != ESP_OK) return false;
        offset += sizeof(segment);
        if (segment.data_len > factory->size - offset || (segment.data_len & 3)) return false;
        offset += segment.data_len;
    }
    return offset < factory->size;
}

extern "C" void initVariant() {
    const esp_partition_t* factory = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    // Preflight lengths before the framework verifier can map/hash corrupt data.
    if (!factory_image_fits(factory)) return;
    const esp_partition_pos_t position = {factory->address, factory->size};
    esp_image_metadata_t image = {};
    // Header magic alone is insufficient: a previous app can cross the factory
    // boundary and have its tail replaced by the current OTA-slot firmware.
    if (esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &position, &image) != ESP_OK) return;
    const esp_partition_t* otadata = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, NULL);
    if (otadata) {
        esp_partition_erase_range(otadata, 0, otadata->size);
    }
}
