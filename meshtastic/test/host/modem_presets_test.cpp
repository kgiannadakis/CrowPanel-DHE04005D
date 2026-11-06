#include "DisplayFormatters.h"
#include "mesh/ModemPresets.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <pb_decode.h>
#include <pb_encode.h>

using Preset = meshtastic_Config_LoRaConfig_ModemPreset;

struct ExpectedPreset {
    Preset preset;
    float bandwidth;
    float wideBandwidth;
    uint8_t sf;
    uint8_t cr;
    const char *name;
};

static void testPresets()
{
    const ExpectedPreset cases[] = {
        {meshtastic_Config_LoRaConfig_ModemPreset_SHORT_TURBO, 500, 1625, 7, 5, "ShortTurbo"},
        {meshtastic_Config_LoRaConfig_ModemPreset_SHORT_FAST, 250, 812.5f, 7, 5, "ShortFast"},
        {meshtastic_Config_LoRaConfig_ModemPreset_SHORT_SLOW, 250, 812.5f, 8, 5, "ShortSlow"},
        {meshtastic_Config_LoRaConfig_ModemPreset_MEDIUM_FAST, 250, 812.5f, 9, 5, "MediumFast"},
        {meshtastic_Config_LoRaConfig_ModemPreset_MEDIUM_SLOW, 250, 812.5f, 10, 5, "MediumSlow"},
        {meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST, 250, 812.5f, 11, 5, "LongFast"},
        {meshtastic_Config_LoRaConfig_ModemPreset_LONG_TURBO, 500, 1625, 11, 8, "LongTurbo"},
        {meshtastic_Config_LoRaConfig_ModemPreset_LONG_MODERATE, 125, 406.25f, 11, 8, "LongMod"},
        {meshtastic_Config_LoRaConfig_ModemPreset_LONG_SLOW, 125, 406.25f, 12, 8, "LongSlow"},
        {meshtastic_Config_LoRaConfig_ModemPreset_NARROW_SLOW, 62.5f, 62.5f, 8, 6, "NarrowSlow"},
    };
    for (const auto &expected : cases) {
        for (const bool wide : {false, true}) {
            float bandwidth = 0;
            uint8_t sf = 0, cr = 0;
            modemPresetToParams(expected.preset, wide, bandwidth, sf, cr);
            assert(bandwidth == (wide ? expected.wideBandwidth : expected.bandwidth));
            assert(sf == expected.sf);
            assert(cr == expected.cr);
            assert(modemPresetToBwKHz(expected.preset, wide) == bandwidth);
            assert(bwCodeToKHz(bwKHzToCode(bandwidth)) == bandwidth);
        }
        assert(std::strcmp(DisplayFormatters::getModemPresetDisplayName(expected.preset, false, true), expected.name) == 0);
        assert(std::strcmp(DisplayFormatters::getModemPresetDisplayName(expected.preset, false, false), "Custom") == 0);
    }
    assert(std::strcmp(DisplayFormatters::getModemPresetDisplayName(
                           meshtastic_Config_LoRaConfig_ModemPreset_NARROW_SLOW, true, true),
                       "NarrowS") == 0);
    assert(bwKHzToCode(62.5f) == 62);
}

static void testExistingFallback()
{
    float bandwidth = 0;
    uint8_t sf = 0, cr = 0;
    modemPresetToParams(meshtastic_Config_LoRaConfig_ModemPreset_VERY_LONG_SLOW, false, bandwidth, sf, cr);
    assert(bandwidth == 250 && sf == 11 && cr == 5);
    assert(std::strcmp(DisplayFormatters::getModemPresetDisplayName(
                           meshtastic_Config_LoRaConfig_ModemPreset_VERY_LONG_SLOW, false, true),
                       "Invalid") == 0);
}

static void testSavedNarrowSlow()
{
    static_assert(meshtastic_Config_LoRaConfig_ModemPreset_NARROW_SLOW == 13, "Keep the official wire enum ID");
    meshtastic_Config_LoRaConfig config = meshtastic_Config_LoRaConfig_init_zero;
    config.use_preset = true;
    config.modem_preset = meshtastic_Config_LoRaConfig_ModemPreset_NARROW_SLOW;
    config.region = meshtastic_Config_LoRaConfig_RegionCode_EU_868;
    config.coding_rate = 0;
    config.bandwidth = bwKHzToCode(modemPresetToBwKHz(config.modem_preset, false));
    config.spread_factor = 8;

    uint8_t bytes[meshtastic_Config_LoRaConfig_size] = {};
    auto output = pb_ostream_from_buffer(bytes, sizeof(bytes));
    assert(pb_encode(&output, meshtastic_Config_LoRaConfig_fields, &config));
    meshtastic_Config_LoRaConfig restored = meshtastic_Config_LoRaConfig_init_zero;
    auto input = pb_istream_from_buffer(bytes, output.bytes_written);
    assert(pb_decode(&input, meshtastic_Config_LoRaConfig_fields, &restored));
    assert(restored.use_preset);
    assert(restored.modem_preset == config.modem_preset);
    assert(restored.region == config.region);
    assert(restored.coding_rate == 0);
    assert(restored.bandwidth == 62 && restored.spread_factor == 8);
    float bandwidth = 0;
    uint8_t sf = 0, cr = 0;
    modemPresetToParams(restored.modem_preset, false, bandwidth, sf, cr);
    assert(bandwidth == 62.5f && sf == 8 && cr == 6);
}

int main()
{
    testPresets();
    testExistingFallback();
    testSavedNarrowSlow();
    std::puts("Modem preset tests passed: NarrowSlow, existing presets, names and protobuf persistence");
}
