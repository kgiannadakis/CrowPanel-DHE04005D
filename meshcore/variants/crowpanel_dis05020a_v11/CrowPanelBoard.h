#pragma once

#include <Arduino.h>
#include <helpers/ESP32Board.h>
#include <helpers/RefCountedDigitalPin.h>

class CrowPanelBoard : public ESP32Board {
public:
#if defined(PIN_VEXT_EN) && (PIN_VEXT_EN >= 0)
  RefCountedDigitalPin periph_power;
  CrowPanelBoard() : periph_power(PIN_VEXT_EN, PIN_VEXT_EN_ACTIVE) {}
#else
  CrowPanelBoard() {}
#endif

  void begin();

  void onBeforeTransmit(void) override;
  void onAfterTransmit(void) override;

  void enterDeepSleep(uint32_t secs, int pin_wake_btn = -1);
  void powerOff() override;

  uint16_t getBattMilliVolts() override;
  const char* getManufacturerName() const override;
};