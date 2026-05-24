#pragma once

#include "EspHal.h"
#include "mesh/RadioLibInterface.h"
#include "SPILock.h"
#include <SPI.h>

class CrowPanelP4Hal : public LockingArduinoHal
{
  public:
    CrowPanelP4Hal(int8_t sck, int8_t miso, int8_t mosi)
        : LockingArduinoHal(SPI, SPISettings(2000000, MSBFIRST, SPI_MODE0))
    {
        _impl.setSpiPins(sck, miso, mosi);
        _impl.setSpiFrequency(2000000);
    }

    void pinMode(uint32_t pin, uint32_t mode) override { _impl.pinMode(pin, mode); }
    void digitalWrite(uint32_t pin, uint32_t value) override { _impl.digitalWrite(pin, value); }
    uint32_t digitalRead(uint32_t pin) override { return _impl.digitalRead(pin); }

    void attachInterrupt(uint32_t interruptNum, void (*cb)(void), uint32_t mode) override
    {
        _impl.attachInterrupt(interruptNum, cb, mode);
    }
    void detachInterrupt(uint32_t interruptNum) override { _impl.detachInterrupt(interruptNum); }

    void delay(RadioLibTime_t ms) override { _impl.delay(ms); }
    void delayMicroseconds(RadioLibTime_t us) override { _impl.delayMicroseconds(us); }
    RadioLibTime_t millis() override { return _impl.millis(); }
    RadioLibTime_t micros() override { return _impl.micros(); }
    long pulseIn(uint32_t pin, uint32_t state, RadioLibTime_t timeout) override
    {
        return _impl.pulseIn(pin, state, timeout);
    }

    void spiBegin() override { _impl.spiBegin(); }
    void spiBeginTransaction() override
    {
        if (spiLock) {
            spiLock->lock();
        }
        _impl.spiBeginTransaction();
    }
    void spiTransfer(uint8_t *out, size_t len, uint8_t *in) override { _impl.spiTransfer(out, len, in); }
    void spiEndTransaction() override
    {
        _impl.spiEndTransaction();
        if (spiLock) {
            spiLock->unlock();
        }
    }
    void spiEnd() override { _impl.spiEnd(); }

    void init() override { _impl.init(); }
    void term() override { _impl.term(); }

  private:
    EspHal _impl;
};
