#include "CrowPanelP4Board.h"
#include <esp_sleep.h>

void CrowPanelP4Board::begin() {
  ESP32Board::begin();
}

void CrowPanelP4Board::onBeforeTransmit(void) {
  // No external PA on this board; SX1262 DIO2 drives the on-module RF switch.
}

void CrowPanelP4Board::onAfterTransmit(void) {
}

void CrowPanelP4Board::powerOff() {
  // No VEXT switch on DHE04005D reference wiring.
}

uint16_t CrowPanelP4Board::getBattMilliVolts() {
  // CHG/BAT ADC not yet characterised on this variant.
  return 0;
}

const char* CrowPanelP4Board::getManufacturerName() const {
  return "Elecrow";
}

void CrowPanelP4Board::enterDeepSleep(uint32_t secs, int pin_wake_btn) {
  powerOff();

  // ESP32-P4 does not provide esp_sleep_enable_ext0_wakeup; use ext1 with a 1-bit mask.
  if (pin_wake_btn >= 0) {
    esp_sleep_enable_ext1_wakeup(1ULL << pin_wake_btn, ESP_EXT1_WAKEUP_ANY_LOW);
  }
  if (secs > 0) {
    esp_sleep_enable_timer_wakeup((uint64_t)secs * 1000000ULL);
  }

  esp_deep_sleep_start();
}
