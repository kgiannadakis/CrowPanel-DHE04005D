#include "CrowPanelStorage.h"
#include "CrowPanelStoragePolicy.h"

#include <LittleFS.h>
#include <esp_log.h>
#include <esp_partition.h>

bool crowpanel_preferences_begin()
{
    static bool attempted = false, mounted = false;
    if (attempted) return mounted;
    attempted = true;

    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "mtdata");
    if (!partition) {
        ESP_LOGE("crowpanel", "Meshtastic mtdata partition missing; no storage modified");
        return false;
    }
    ESP_LOGI("crowpanel", "mtdata offset=0x%lx size=0x%lx",
             (unsigned long)partition->address, (unsigned long)partition->size);
    if (LittleFS.begin(false, "/littlefs", 10, "mtdata")) return mounted = true;

    // A blank first-boot partition also reports a corrupt LittleFS superblock.
    // Check every byte, not just its header, before allowing initialization.
    const bool erased = crowpanel_storage::partitionIsErased(partition->size,
        [partition](size_t offset, uint8_t *buffer, size_t length) {
            return esp_partition_read(partition, offset, buffer, length) == ESP_OK;
        });
    if (!erased) {
        ESP_LOGE("crowpanel", "mtdata is nonblank or unreadable; refusing to format existing data");
        return false;
    }
    ESP_LOGW("crowpanel", "mtdata verified entirely erased; initializing first-boot LittleFS");
    mounted = LittleFS.begin(true, "/littlefs", 10, "mtdata");
    if (!mounted) ESP_LOGE("crowpanel", "First-boot LittleFS initialization failed");
    return mounted;
}
