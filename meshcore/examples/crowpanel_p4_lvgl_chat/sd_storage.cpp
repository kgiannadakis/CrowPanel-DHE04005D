// ============================================================
// sd_storage.cpp — microSD mount via ESP-IDF SDMMC
// ------------------------------------------------------------
// Ported from Elecrow's Lesson08 reference. We keep the same slot
// (SDMMC_HOST_SLOT_0), same 1-bit width, and same 10 MHz cap —
// deviating from known-good timings on this class of board usually
// costs us a week of debugging. The original example crashes hard
// on missing card (its error path is `while(1) delay(1000)`); we
// replace that with a soft failure so the firmware stays usable
// without an SD inserted.
// ============================================================

#include "sd_storage.h"
#include "board_config.h"
#include "utils.h"       // serialmon_append

#include <esp_err.h>
#include <esp_vfs_fat.h>
#include <driver/sdmmc_host.h>
#include <sdmmc_cmd.h>

static sdmmc_card_t* s_card    = nullptr;
static bool          s_mounted = false;

bool sd_is_mounted() { return s_mounted; }

uint64_t sd_total_bytes() {
  if (!s_mounted || !s_card) return 0;
  // sdmmc_card_t's CSD holds sector count + sector size; IDF 5.5's
  // sdmmc_csd_t has had these two fields stably named since 4.x.
  return (uint64_t)s_card->csd.capacity * (uint64_t)s_card->csd.sector_size;
}

uint64_t sd_free_bytes() {
  // statvfs() isn't shipped in arduino-esp32's newlib build, and
  // FatFS's f_getfree needs drive-number plumbing that esp_vfs_fat
  // doesn't expose. Free-space reporting isn't essential for Phase 1
  // (maps + emoji both only READ from the card), so this is a stub.
  // If a feature later needs it, add an f_getfree helper behind a
  // small wrapper that pokes esp_vfs_fat's internal drive number.
  return 0;
}

bool sd_init() {
  if (s_mounted) return true;

  esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {};
  // Never auto-format: if a card is present but its filesystem is
  // corrupt, we want to know — silently reformatting someone's card
  // would be hostile.
  mount_cfg.format_if_mount_failed = false;
  mount_cfg.max_files              = 5;
  mount_cfg.allocation_unit_size   = 16 * 1024;

  sdmmc_host_t host  = SDMMC_HOST_DEFAULT();
  host.slot          = SDMMC_HOST_SLOT_0;
  // 25 MHz is the SD-spec Default Speed ceiling for 1-bit mode. Elecrow's
  // example used 10 MHz to be ultra-conservative; bumping to 25 MHz cuts
  // atlas load time (~1 MB of emoji data today, more with the expanded
  // atlas, and map tiles coming next) by ~2.5x. The SDMMC host driver
  // negotiates down automatically if the card or signal-integrity can't
  // hold 25 MHz - no manual retry needed.
  host.max_freq_khz  = 25000;

  sdmmc_slot_config_t slot_cfg = SDMMC_SLOT_CONFIG_DEFAULT();
  slot_cfg.clk    = (gpio_num_t)SD_GPIO_MMC_CLK;
  slot_cfg.cmd    = (gpio_num_t)SD_GPIO_MMC_CMD;
  slot_cfg.d0     = (gpio_num_t)SD_GPIO_MMC_D0;
  slot_cfg.width  = 1;   // 1-bit SDIO — D1/D2/D3 aren't routed on this board
  slot_cfg.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

  esp_err_t err = esp_vfs_fat_sdmmc_mount(
      SD_MOUNT_POINT, &host, &slot_cfg, &mount_cfg, &s_card);

  if (err != ESP_OK) {
    char buf[96];
    if (err == ESP_FAIL) {
      snprintf(buf, sizeof(buf), "SD: no FAT filesystem on card (format it first)");
    } else {
      snprintf(buf, sizeof(buf), "SD: not mounted (%s) - check card + pullups",
               esp_err_to_name(err));
    }
    serialmon_append(buf);
    s_card    = nullptr;
    s_mounted = false;
    return false;
  }

  s_mounted = true;

  char msg[96];
  uint64_t total_mb = sd_total_bytes() / (1024 * 1024);
  snprintf(msg, sizeof(msg), "SD mounted at %s - %llu MB", SD_MOUNT_POINT,
           (unsigned long long)total_mb);
  serialmon_append(msg);
  return true;
}
