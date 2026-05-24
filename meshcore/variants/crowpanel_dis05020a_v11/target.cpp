// variants/crowpanel_dis05020a_v11/target.cpp  (CLEAN - only debugging removed)

#include "target.h"

#include <SPI.h>
#include <SPIFFS.h>
#include <esp_system.h>   // esp_random()

// ---------- Board ----------
CrowPanelBoard board;

// ---------- Minimal RTCClock (millis-based + settable epoch) ----------
class CrowPanelRTCClock : public mesh::RTCClock {
  uint32_t _base_epoch  = 0;   // UTC epoch set via setCurrentTime()
  uint32_t _base_millis = 0;   // millis() at the moment it was set
public:
  void tick() override {}

  uint32_t getCurrentTime() override {
    if (_base_epoch == 0) return millis() / 1000;
    return _base_epoch + (millis() - _base_millis) / 1000;
  }

  void setCurrentTime(uint32_t time) override {
    _base_epoch  = time;
    _base_millis = millis();
  }
};

static CrowPanelRTCClock _rtc_impl;
mesh::RTCClock& rtc_clock = _rtc_impl;

// ---------- SX1262 (RadioLib) + MeshCore wrapper ----------
static SPIClass loraSPI(FSPI);

// Bind Module to THIS SPI instance (prevents -2 + avoids double SPI init issues)
static Module loraModule(
  P_LORA_NSS,      // CS
  P_LORA_DIO_1,    // DIO1
  P_LORA_RESET,    // RST
  P_LORA_BUSY,     // BUSY
  loraSPI          // SPI instance
);

static CustomSX1262 loraRadio(&loraModule);
CustomSX1262Wrapper radio_driver(loraRadio, board);

// ---------- Required by MeshCore examples ----------
bool radio_init() {
  board.begin();
  delay(10);

  // SPIFFS for identity keys + state
  if (!SPIFFS.begin(false)) {
    Serial.println("SPIFFS mount failed, formatting...");
    SPIFFS.format();
    if (!SPIFFS.begin(true)) {
      Serial.println("SPIFFS mount failed even after format");
    } else {
      Serial.println("SPIFFS formatted + mounted OK");
    }
  } else {
    Serial.println("SPIFFS mounted OK");
  }

  // CustomSX1262::std_init(&spi) will call spi->begin(P_LORA_*) exactly once.
  if (!loraRadio.std_init(&loraSPI)) {
    return false;
  }

  // Keep pin modes (harmless; helps if DIO is open-drain)
  pinMode(P_LORA_DIO_1, INPUT_PULLUP);
  pinMode(P_LORA_BUSY, INPUT);

  // MeshCore interop settings
  loraRadio.setCRC(true);

  // Route IRQs to DIO1 early
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

  // keep interop settings
  loraRadio.setCRC(true);

  // Keep IRQ routing consistent
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