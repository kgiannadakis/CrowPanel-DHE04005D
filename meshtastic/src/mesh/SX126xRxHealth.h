#pragma once

#include <stdint.h>
#include <TypeDef.h>
#include <modules/SX126x/SX126x_commands.h>
#include <modules/SX126x/SX126x_registers.h>

namespace sx126x_rx_health
{
enum class Result { Deferred, Verified, Restored, Failed };

inline uint32_t staleActivityFlags(uint32_t irq, bool active)
{
    if ((irq & RADIOLIB_SX126X_IRQ_RX_DONE) || (active && !(irq & RADIOLIB_SX126X_IRQ_HEADER_ERR)))
        return 0;
    return irq & (RADIOLIB_SX126X_IRQ_PREAMBLE_DETECTED | RADIOLIB_SX126X_IRQ_HEADER_VALID |
                  RADIOLIB_SX126X_IRQ_HEADER_ERR | RADIOLIB_SX126X_IRQ_SYNC_WORD_VALID);
}

template <typename Radio, typename IsIdle>
Result verify(Radio &radio, IsIdle isIdle, bool boosted, bool patchEnabled, int16_t &error)
{
    error = RADIOLIB_ERR_NONE;
    if (!isIdle())
        return Result::Deferred;

    uint8_t gain = 0;
    uint8_t patch = 0;
    error = radio.readRegister(RADIOLIB_SX126X_REG_RX_GAIN, &gain, 1);
    if (error != RADIOLIB_ERR_NONE)
        return Result::Failed;
    // Do not mistake an invalid SPI response for a lost gain setting.
    if (gain != RADIOLIB_SX126X_RX_GAIN_BOOSTED && gain != RADIOLIB_SX126X_RX_GAIN_POWER_SAVING) {
        error = RADIOLIB_ERR_UNKNOWN;
        return Result::Failed;
    }
    if (patchEnabled) {
        error = radio.readRegister(0x8B5, &patch, 1);
        if (error != RADIOLIB_ERR_NONE)
            return Result::Failed;
    }

    const uint8_t expected = boosted ? RADIOLIB_SX126X_RX_GAIN_BOOSTED : RADIOLIB_SX126X_RX_GAIN_POWER_SAVING;
    const bool repairGain = gain != expected;
    const bool repairPatch = patchEnabled && !(patch & 0x01);
    if (!repairGain && !repairPatch)
        return Result::Verified;
    if (!isIdle())
        return Result::Deferred;

    if (repairGain) {
        error = radio.setRxBoostedGainMode(boosted, true);
        if (error != RADIOLIB_ERR_NONE)
            return Result::Failed;
    }
    if (repairPatch) {
        if (!isIdle())
            return Result::Deferred;
        patch |= 0x01;
        error = radio.writeRegister(0x8B5, &patch, 1);
        if (error != RADIOLIB_ERR_NONE)
            return Result::Failed;
    }

    error = radio.readRegister(RADIOLIB_SX126X_REG_RX_GAIN, &gain, 1);
    if (error == RADIOLIB_ERR_NONE && patchEnabled)
        error = radio.readRegister(0x8B5, &patch, 1);
    if (error != RADIOLIB_ERR_NONE)
        return Result::Failed;
    if (gain != expected || (patchEnabled && !(patch & 0x01))) {
        error = RADIOLIB_ERR_SPI_WRITE_FAILED;
        return Result::Failed;
    }
    return Result::Restored;
}
} // namespace sx126x_rx_health
