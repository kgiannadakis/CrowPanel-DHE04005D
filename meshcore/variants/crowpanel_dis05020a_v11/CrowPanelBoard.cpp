#include "CrowPanelBoard.h"
#include <esp_sleep.h>

void CrowPanelBoard::begin() {
#if defined(PIN_VEXT_EN) && (PIN_VEXT_EN >= 0)
  // Enable external peripherals if your board has a VEXT switch.
  periph_power.ref();
#endif

  // Let MeshCore's ESP32Board bring up radio, RNG, RTC, etc.
  ESP32Board::begin();
}

void CrowPanelBoard::onBeforeTransmit(void) {
  // If you later add an RF front-end enable pin, set it HIGH here.
}

void CrowPanelBoard::onAfterTransmit(void) {
  // If you later add an RF front-end enable pin, set it LOW here.
}

void CrowPanelBoard::powerOff() {
#if defined(PIN_VEXT_EN) && (PIN_VEXT_EN >= 0)
  periph_power.unref();
#endif
}

uint16_t CrowPanelBoard::getBattMilliVolts() {
  // CrowPanel battery measurement not wired yet → return 0 for now.
  return 0;
}

const char* CrowPanelBoard::getManufacturerName() const {
  return "Elecrow";
}

void CrowPanelBoard::enterDeepSleep(uint32_t secs, int pin_wake_btn) {
  powerOff();

  if (pin_wake_btn >= 0) {
    esp_sleep_enable_ext0_wakeup((gpio_num_t)pin_wake_btn, 0);
  }
  if (secs > 0) {
    esp_sleep_enable_timer_wakeup((uint64_t)secs * 1000000ULL);
  }

  esp_deep_sleep_start();
}