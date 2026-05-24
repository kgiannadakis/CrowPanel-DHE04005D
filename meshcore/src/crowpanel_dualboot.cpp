// =============================================================================
// crowpanel_dualboot.cpp
//
// Erases otadata on every boot → bootloader falls back to factory (the boot
// selector) on the next reset, so the user can pick a different firmware.
//
// NOTE: this file used to also pre-mount SPIFFS on the "mcdata" partition,
// which was correct for the V11 build. The P4 chat build uses LittleFS on
// that same partition (see main.cpp's setup()). Mounting both formats on
// the same partition causes them to reformat each other on every boot,
// wiping channels/contacts/identity. Keep this file FS-agnostic.
// =============================================================================

#include <Arduino.h>
#include <esp_partition.h>

extern "C" void initVariant() {
    const esp_partition_t* otadata = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, NULL);
    if (otadata) {
        esp_partition_erase_range(otadata, 0, otadata->size);
    }
}
