#pragma once

#include "MemoryPool.h"
#include "MeshTypes.h"
#include "ModemPresets.h"
#include "PointerQueue.h"
#include "configuration.h"
#include "detect/LoRaRadioType.h"

// Sentinel marking the end of a modem preset array. Declared `const` rather
// than `constexpr` because the cast from 0xFF to the enum is out-of-range and
// therefore not a valid constant expression on Clang 16+ (Apple Clang on
// macOS). The value is only ever compared at runtime, so static-init is fine.
static const meshtastic_Config_LoRaConfig_ModemPreset MODEM_PRESET_END =
    static_cast<meshtastic_Config_LoRaConfig_ModemPreset>(0xFF);

// Region profile: bundles the preset list with regulatory parameters shared across regions
struct RegionProfile {
    const meshtastic_Config_LoRaConfig_ModemPreset *presets; // sentinel-terminated; first entry is the default
    float spacing;                                           // gaps between radio channels
    float padding;                                           // padding at each side of the "operating channel"
    bool audioPermitted;
    bool licensedOnly;        // a region profile for licensed operators only
    int8_t textThrottle;      // throttle for text - future expansion
    int8_t positionThrottle;  // throttle for location data - future expansion
    int8_t telemetryThrottle; // throttle for telemetry - future expansion
    uint8_t overrideSlot;     // a per-region override slot for if we need to fix it in place
};

extern const RegionProfile PROFILE_STD;
extern const RegionProfile PROFILE_EU868;
extern const RegionProfile PROFILE_UNDEF;
// extern const RegionProfile  PROFILE_LITE;
// extern const RegionProfile  PROFILE_NARROW;
// extern const RegionProfile  PROFILE_HAM;

// Map from old region names to new region enums
struct RegionInfo {
    meshtastic_Config_LoRaConfig_RegionCode code;
    float freqStart;
    float freqEnd;
    float dutyCycle;
    float spacing;
    uint8_t powerLimit; // Or zero for not set
    bool audioPermitted;
    bool freqSwitching;
    bool wideLora;
    const char *name; // EU433 etc
};

extern const RegionInfo regions[];
extern const RegionInfo *myRegion;

extern void initRegion();

// Valid LoRa spread factor range and defaults
constexpr uint8_t LORA_SF_MIN = 5;
constexpr uint8_t LORA_SF_MAX = 12;
constexpr uint8_t LORA_SF_DEFAULT = 11; // LONG_FAST default

// Valid LoRa coding rate range and default
constexpr uint8_t LORA_CR_MIN = 4;
constexpr uint8_t LORA_CR_MAX = 8;
constexpr uint8_t LORA_CR_DEFAULT = 5; // LONG_FAST default

// Default bandwidth in kHz (LONG_FAST)
constexpr float LORA_BW_DEFAULT_KHZ = 250.0f;

/// Clamp spread factor to the valid LoRa range [5, 12].
/// Out-of-range values (including 0 from unset preset mode) return LORA_SF_DEFAULT.
static inline uint8_t clampSpreadFactor(uint8_t sf)
{
    // We check for RF95 radios that are incompatible with Spreading Factors 5 and 6.
    if (radioType == RF95_RADIO && (sf == 5 || sf == 6))
        return LORA_SF_DEFAULT;

    if (sf < LORA_SF_MIN || sf > LORA_SF_MAX)
        return LORA_SF_DEFAULT;
    return sf;
}

/// Clamp coding rate to the valid LoRa range [4, 8].
/// Out-of-range values return LORA_CR_DEFAULT.
static inline uint8_t clampCodingRate(uint8_t cr)
{
    if (cr < LORA_CR_MIN || cr > LORA_CR_MAX)
        return LORA_CR_DEFAULT;
    return cr;
}

/// Ensure bandwidth is positive. Non-positive values return LORA_BW_DEFAULT_KHZ.
static inline float clampBandwidthKHz(float bwKHz)
{
    if (bwKHz <= 0.0f)
        return LORA_BW_DEFAULT_KHZ;
    return bwKHz;
}
