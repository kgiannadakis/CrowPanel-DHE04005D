// variants/crowpanel_dhe04005d/target.cpp
//
// MeshCore target glue for Elecrow CrowPanel DHE04005D (ESP32-P4 + SX1262).
//
// IMPORTANT: on arduino-esp32 3.x for ESP32-P4, SPIClass does NOT reliably
// route MISO through the GPIO matrix — every SPI read returns 0x00 even
// though MOSI/SCK work. Elecrow's own IDF Lesson14 example sidesteps this
// by using RadioLib's native RadioLibHal against raw ESP-IDF spi_master
// on SPI3_HOST. We do the same here. See EspHal.h in this folder — copied
// verbatim from their Lesson14 reference.

#include "target.h"
#include <RadioLib.h>
#include "EspHal.h"
#include "ds1307.h"

#include <LittleFS.h>
#include <esp_system.h>   // esp_random()
#include <esp_partition.h>

// ---------- Board ----------
CrowPanelP4Board board;

// ---------- RTCClock backed by DS3231 (driver in ds1307.cpp) ----------
// Hot-path getCurrentTime() avoids I2C every call by tracking elapsed
// millis from a base epoch. The base is refreshed periodically in
// tick() to absorb any drift between the chip's oscillator and our own
// millis() timer. Writes to setCurrentTime persist into the
// battery-backed RTC so the clock survives reboots and unplugs.
//
// If the chip is absent or its OSF (oscillator-stopped) flag is set
// (fresh chip with no battery, or VBAT brown-out), we fall back to a
// volatile clock — time starts at 0 (or the first setCurrentTime call,
// e.g. from NTP) and won't persist across reboot until the next sync.
class CrowPanelP4RTCClock : public mesh::RTCClock {
  uint32_t _base_epoch   = 0;
  uint32_t _base_millis  = 0;
  uint32_t _last_sync_ms = 0;
  // Re-read hw clock every 60 s in tick() to stay in sync with the
  // battery-backed RTC even if millis() drifts.
  static constexpr uint32_t kSyncIntervalMs = 60 * 1000;
public:
  void init_from_hw() {
    uint32_t utc = 0;
    if (ds1307_present() && ds1307_read_unix(&utc) && utc > 0) {
      _base_epoch   = utc;
      _base_millis  = millis();
      _last_sync_ms = _base_millis;
      Serial.printf("RTC: time loaded from chip (epoch=%u)\n", (unsigned)utc);
    } else {
      Serial.println("RTC: chip has no valid time yet (waiting for NTP sync)");
    }
  }
  void tick() override {
    if (_base_epoch == 0) return;
    uint32_t now = millis();
    if ((now - _last_sync_ms) < kSyncIntervalMs) return;
    _last_sync_ms = now;
    uint32_t hw_utc = 0;
    if (ds1307_present() && ds1307_read_unix(&hw_utc) && hw_utc > 0) {
      _base_epoch  = hw_utc;
      _base_millis = now;
    }
  }
  uint32_t getCurrentTime() override {
    if (_base_epoch == 0) return millis() / 1000;
    return _base_epoch + (millis() - _base_millis) / 1000;
  }
  void setCurrentTime(uint32_t time) override {
    _base_epoch   = time;
    _base_millis  = millis();
    _last_sync_ms = _base_millis;
    if (!ds1307_present()) return;
    if (!ds1307_write_unix(time)) {
      Serial.println("RTC: chip write FAILED (I2C error)");
      return;
    }
    // No verify-readback here. The boot-time init_from_hw() already
    // proves the chip can both read and write (we round-trip time on
    // every reboot). Re-verifying on every NTP sync added ~stack and
    // two extra I2C transactions on wifi_ntp_task — and crashed under
    // its 2 KB stack limit before we bumped to 6 KB. The single Serial
    // line is kept so the log still shows when NTP overwrites the
    // hardware clock.
    Serial.printf("RTC: chip set to %u\n", (unsigned)time);
  }
};

static CrowPanelP4RTCClock _rtc_impl;
mesh::RTCClock& rtc_clock = _rtc_impl;

// ---------- SX1262 via RadioLib + native ESP-IDF HAL ----------
// EspHal wraps the IDF spi_master driver on SPI3_HOST. No Arduino SPIClass.
static EspHal loraHal;

// Note the constructor shape: with a HAL pointer, Module takes
// (HAL*, CS, IRQ, RST, GPIO/BUSY). No SPIClass arg.
static Module loraModule(&loraHal, P_LORA_NSS, P_LORA_DIO_1, P_LORA_RESET, P_LORA_BUSY);
static CustomSX1262 loraRadio(&loraModule);
CustomSX1262Wrapper radio_driver(loraRadio, board);

// ---------- Required by MeshCore examples ----------
bool radio_init() {
  board.begin();
  delay(10);

  // Bring up the battery-backed RTC on the shared I2C bus. If the chip
  // isn't fitted (or has a dead button cell), ds1307_begin returns
  // false and the clock falls back to volatile mode.
  if (ds1307_begin()) {
    _rtc_impl.init_from_hw();
  }

  // --- Pin sanity probe ---
  pinMode(P_LORA_BUSY,  INPUT);
  pinMode(P_LORA_DIO_1, INPUT);
  pinMode(P_LORA_NSS,   INPUT_PULLUP);
  pinMode(P_LORA_RESET, INPUT_PULLUP);
  pinMode(P_LORA_MISO,  INPUT_PULLUP);
  delay(5);
  Serial.printf("SX1262 pin probe: BUSY=%d DIO1=%d NSS=%d RESET=%d MISO=%d\n",
                digitalRead(P_LORA_BUSY),
                digitalRead(P_LORA_DIO_1),
                digitalRead(P_LORA_NSS),
                digitalRead(P_LORA_RESET),
                digitalRead(P_LORA_MISO));

  // --- Diagnostic: list every partition the running IDF can see. ---
  Serial.println("--- Partitions visible to app ---");
  esp_partition_iterator_t it = esp_partition_find(
      ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, nullptr);
  while (it) {
    const esp_partition_t* p = esp_partition_get(it);
    Serial.printf("  %-10s type=0x%02X sub=0x%02X offset=0x%08X size=%u KB\n",
                  p->label, p->type, p->subtype, (unsigned)p->address,
                  (unsigned)(p->size / 1024));
    it = esp_partition_next(it);
  }
  esp_partition_iterator_release(it);
  Serial.println("--- end partitions ---");

  // --- LittleFS for identity keys + persistent state ---
  // We deliberately use LittleFS instead of SPIFFS on this 10 MB partition:
  // SPIFFS format on a partition this large takes ~2 minutes (bulk erase of
  // every block) and trips the task watchdog. LittleFS formats in ms because
  // it only writes superblock metadata.
  //
  // The partition is labelled `mcdata` (subtype `spiffs`) in the shared
  // dualboot table — see partitions_p4.csv / selector/partitions_dualboot.csv.
  // Arduino's LittleFS library happily mounts a spiffs-subtype partition by
  // name. The label MUST match the partition table, otherwise mount fails
  // silently and every write is lost on reboot.
  if (LittleFS.begin(false, "/littlefs", 10, "mcdata")) {
    Serial.println("LittleFS mounted OK");
  } else {
    Serial.println("LittleFS empty — formatting (first-boot only, fast)...");
    if (LittleFS.begin(true, "/littlefs", 10, "mcdata")) {
      Serial.println("LittleFS formatted + mounted OK");
    } else {
      Serial.println("LittleFS mount failed — continuing without persistent storage");
    }
  }

  // --- Route pins into the native IDF HAL on SPI3_HOST ---
  loraHal.setSpiPins(P_LORA_SCLK, P_LORA_MISO, P_LORA_MOSI);
  loraHal.setSpiFrequency(2000000);   // 2 MHz — conservative for first bring-up

  // --- Manual reset pulse (belt-and-braces; RadioLib will also do one) ---
  pinMode(P_LORA_RESET, OUTPUT);
  digitalWrite(P_LORA_RESET, LOW);
  delay(5);
  digitalWrite(P_LORA_RESET, HIGH);
  delay(10);

  // --- RadioLib begin (equivalent of CustomSX1262::std_init minus the
  // Arduino SPIClass setup that we're bypassing) ---
#ifdef SX126X_DIO3_TCXO_VOLTAGE
  float tcxo = SX126X_DIO3_TCXO_VOLTAGE;
#else
  float tcxo = 1.6f;
#endif
  uint8_t cr = LORA_CR;

  int status = loraRadio.begin(LORA_FREQ, LORA_BW, LORA_SF, cr,
                               RADIOLIB_SX126X_SYNC_WORD_PRIVATE,
                               LORA_TX_POWER, 16, tcxo);
  if (status == RADIOLIB_ERR_SPI_CMD_FAILED || status == RADIOLIB_ERR_SPI_CMD_INVALID) {
    // Retry with TCXO disabled for modules without an external TCXO.
    status = loraRadio.begin(LORA_FREQ, LORA_BW, LORA_SF, cr,
                             RADIOLIB_SX126X_SYNC_WORD_PRIVATE,
                             LORA_TX_POWER, 16, 0.0f);
  }
  if (status != RADIOLIB_ERR_NONE) {
    Serial.printf("ERROR: radio init failed: %d\n", status);
    return false;
  }

  loraRadio.setCRC(1);
#ifdef SX126X_DIO2_AS_RF_SWITCH
  loraRadio.setDio2AsRfSwitch(SX126X_DIO2_AS_RF_SWITCH);
#endif

  // MeshCore interop — route IRQs to DIO1.
  loraRadio.setDioIrqParams(
    RADIOLIB_SX126X_IRQ_ALL,
    RADIOLIB_SX126X_IRQ_ALL,
    RADIOLIB_SX126X_IRQ_NONE,
    RADIOLIB_SX126X_IRQ_NONE
  );
  return true;
}

void radio_set_params(float freq_mhz, float bw_khz, uint8_t sf, uint8_t cr) {
  loraRadio.setFrequency(freq_mhz);
  loraRadio.setBandwidth(bw_khz);
  loraRadio.setSpreadingFactor(sf);
  if (cr < 5) cr = 5;
  if (cr > 8) cr = 8;
  loraRadio.setCodingRate(cr);
  loraRadio.setCRC(true);
  loraRadio.setDioIrqParams(
    RADIOLIB_SX126X_IRQ_ALL,
    RADIOLIB_SX126X_IRQ_ALL,
    RADIOLIB_SX126X_IRQ_NONE,
    RADIOLIB_SX126X_IRQ_NONE
  );
}

void radio_set_tx_power(int8_t dbm) {
  loraRadio.setOutputPower(dbm);
}

uint32_t radio_get_rng_seed() {
  uint32_t s = (uint32_t)esp_random();
  s ^= (uint32_t)micros();
  return s;
}
