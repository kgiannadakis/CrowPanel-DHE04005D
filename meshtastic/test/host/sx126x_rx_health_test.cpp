#include "mesh/SX126xRxHealth.h"
#include <assert.h>
#include <initializer_list>
#include <stdio.h>

using sx126x_rx_health::Result;

struct FakeRadio {
    uint8_t gain = RADIOLIB_SX126X_RX_GAIN_BOOSTED;
    uint8_t patch = 0xA1;
    unsigned reads = 0, gainWrites = 0, patchWrites = 0;
    unsigned failRead = 0;
    bool persistent = false, ignoreGainWrite = false, ignorePatchWrite = false;
    int16_t gainError = 0, patchError = 0;

    int16_t readRegister(uint16_t address, uint8_t *value, uint8_t size)
    {
        assert(size == 1);
        if (++reads == failRead)
            return RADIOLIB_ERR_SPI_CMD_FAILED;
        assert(address == RADIOLIB_SX126X_REG_RX_GAIN || address == 0x8B5);
        *value = address == RADIOLIB_SX126X_REG_RX_GAIN ? gain : patch;
        return RADIOLIB_ERR_NONE;
    }

    int16_t setRxBoostedGainMode(bool boosted, bool persist)
    {
        ++gainWrites;
        persistent = persist;
        if (gainError)
            return gainError;
        if (!ignoreGainWrite)
            gain = boosted ? RADIOLIB_SX126X_RX_GAIN_BOOSTED : RADIOLIB_SX126X_RX_GAIN_POWER_SAVING;
        return RADIOLIB_ERR_NONE;
    }

    int16_t writeRegister(uint16_t address, const uint8_t *value, uint8_t size)
    {
        assert(address == 0x8B5 && size == 1);
        ++patchWrites;
        if (patchError)
            return patchError;
        if (!ignorePatchWrite)
            patch = *value;
        return RADIOLIB_ERR_NONE;
    }
};

static Result check(FakeRadio &radio, int16_t &error, bool boosted = true, bool patch = true)
{
    return sx126x_rx_health::verify(radio, [] { return true; }, boosted, patch, error);
}

int main()
{
    int16_t error = 0;
    {
        FakeRadio radio;
        assert(check(radio, error) == Result::Verified && error == 0);
        assert(radio.gainWrites == 0 && radio.patchWrites == 0 && radio.reads == 2);
    }
    {
        FakeRadio radio;
        radio.gain = RADIOLIB_SX126X_RX_GAIN_POWER_SAVING;
        assert(check(radio, error) == Result::Restored);
        assert(radio.gain == RADIOLIB_SX126X_RX_GAIN_BOOSTED && radio.persistent);
        assert(radio.gainWrites == 1 && radio.patchWrites == 0);
    }
    {
        FakeRadio radio;
        radio.patch = 0xA0;
        assert(check(radio, error) == Result::Restored);
        assert(radio.patch == 0xA1 && radio.patchWrites == 1 && radio.gainWrites == 0);
    }
    {
        FakeRadio radio;
        radio.gain = RADIOLIB_SX126X_RX_GAIN_POWER_SAVING;
        radio.patch = 0xA0;
        assert(check(radio, error) == Result::Restored);
        assert(radio.gainWrites == 1 && radio.patchWrites == 1);
    }
    {
        FakeRadio radio;
        assert(check(radio, error, false) == Result::Restored);
        assert(radio.gain == RADIOLIB_SX126X_RX_GAIN_POWER_SAVING && radio.persistent);
        assert(check(radio, error, false) == Result::Verified);
    }
    {
        FakeRadio radio;
        radio.patch = 0;
        assert(check(radio, error, true, false) == Result::Verified);
        assert(radio.reads == 1 && radio.patchWrites == 0);
    }
    for (uint8_t invalid : {uint8_t(0), uint8_t(0xFF), uint8_t(0x84)}) {
        FakeRadio radio;
        radio.gain = invalid;
        assert(check(radio, error) == Result::Failed && error != 0);
        assert(radio.gainWrites == 0 && radio.patchWrites == 0);
    }
    for (unsigned failedRead = 1; failedRead <= 2; ++failedRead) {
        FakeRadio radio;
        radio.failRead = failedRead;
        assert(check(radio, error) == Result::Failed && error == RADIOLIB_ERR_SPI_CMD_FAILED);
        assert(radio.gainWrites == 0 && radio.patchWrites == 0);
    }
    {
        FakeRadio radio;
        assert(sx126x_rx_health::verify(radio, [] { return false; }, true, true, error) == Result::Deferred);
        assert(radio.reads == 0 && radio.gainWrites == 0 && radio.patchWrites == 0);
    }
    {
        FakeRadio radio;
        radio.gain = RADIOLIB_SX126X_RX_GAIN_POWER_SAVING;
        unsigned idleChecks = 0;
        auto idle = [&] { return ++idleChecks < 2; };
        assert(sx126x_rx_health::verify(radio, idle, true, true, error) == Result::Deferred);
        assert(radio.gainWrites == 0 && radio.patchWrites == 0);
    }
    {
        FakeRadio radio;
        radio.gain = RADIOLIB_SX126X_RX_GAIN_POWER_SAVING;
        radio.patch = 0xA0;
        unsigned idleChecks = 0;
        auto idle = [&] { return ++idleChecks < 3; };
        assert(sx126x_rx_health::verify(radio, idle, true, true, error) == Result::Deferred);
        assert(radio.gainWrites == 1 && radio.patchWrites == 0);
        assert(check(radio, error) == Result::Restored);
        assert(radio.gainWrites == 1 && radio.patchWrites == 1);
    }
    for (bool patchFailure : {false, true}) {
        FakeRadio radio;
        radio.gain = RADIOLIB_SX126X_RX_GAIN_POWER_SAVING;
        radio.patch = 0xA0;
        if (patchFailure)
            radio.patchError = RADIOLIB_ERR_SPI_CMD_FAILED;
        else
            radio.gainError = RADIOLIB_ERR_SPI_CMD_FAILED;
        assert(check(radio, error) == Result::Failed && error == RADIOLIB_ERR_SPI_CMD_FAILED);
    }
    for (bool patchFailure : {false, true}) {
        FakeRadio radio;
        radio.gain = RADIOLIB_SX126X_RX_GAIN_POWER_SAVING;
        radio.patch = 0xA0;
        radio.ignoreGainWrite = !patchFailure;
        radio.ignorePatchWrite = patchFailure;
        assert(check(radio, error) == Result::Failed && error == RADIOLIB_ERR_SPI_WRITE_FAILED);
    }
    for (unsigned failedRead = 3; failedRead <= 4; ++failedRead) {
        FakeRadio radio;
        radio.gain = RADIOLIB_SX126X_RX_GAIN_POWER_SAVING;
        radio.failRead = failedRead;
        assert(check(radio, error) == Result::Failed && error == RADIOLIB_ERR_SPI_CMD_FAILED);
    }

    constexpr uint32_t activity = RADIOLIB_SX126X_IRQ_PREAMBLE_DETECTED | RADIOLIB_SX126X_IRQ_HEADER_VALID |
                                  RADIOLIB_SX126X_IRQ_HEADER_ERR | RADIOLIB_SX126X_IRQ_SYNC_WORD_VALID;
    constexpr uint32_t terminal = RADIOLIB_SX126X_IRQ_RX_DONE | RADIOLIB_SX126X_IRQ_TX_DONE |
                                  RADIOLIB_SX126X_IRQ_CRC_ERR | RADIOLIB_SX126X_IRQ_TIMEOUT;
    assert(sx126x_rx_health::staleActivityFlags(activity | terminal, false) == 0);
    assert(sx126x_rx_health::staleActivityFlags(RADIOLIB_SX126X_IRQ_PREAMBLE_DETECTED, true) == 0);
    assert(sx126x_rx_health::staleActivityFlags(RADIOLIB_SX126X_IRQ_HEADER_VALID, true) == 0);
    assert(sx126x_rx_health::staleActivityFlags(activity, false) == activity);
    assert(sx126x_rx_health::staleActivityFlags(activity, true) == activity);
    assert(sx126x_rx_health::staleActivityFlags(terminal, false) == 0);
    assert(sx126x_rx_health::staleActivityFlags(RADIOLIB_SX126X_IRQ_PREAMBLE_DETECTED, false) ==
           RADIOLIB_SX126X_IRQ_PREAMBLE_DETECTED);
    for (uint32_t irq = 0; irq < 1024; ++irq) {
        for (bool active : {false, true}) {
            uint32_t cleared = sx126x_rx_health::staleActivityFlags(irq, active);
            assert((cleared & ~activity) == 0);
            if ((irq & RADIOLIB_SX126X_IRQ_RX_DONE) || (active && !(irq & RADIOLIB_SX126X_IRQ_HEADER_ERR)))
                assert(cleared == 0);
        }
    }
    puts("SX126x RX health tests passed: gain/patch repair, idle deferral, SPI failures and completed-packet protection");
}
